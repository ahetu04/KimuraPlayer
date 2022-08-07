//
// Copyright (c) Alexandre Hetu.
// Licensed under the MIT License.
//
// https://github.com/ahetu04
//

#include "KimuraConverter.h"
#include "Threadpool.h"
#include <filesystem>
#include <type_traits>

#include "Include/IKimuraConverter.h"

using namespace std::chrono_literals; 

using namespace Alembic::AbcGeom;
using namespace Alembic::Abc;

using namespace Kimura;

#ifdef SUPPORT_IMAGE_SEQUENCES
	#include "TexconvKimura.h"
#endif

//-----------------------------------------------------------------------------
// Kimura::CreateConverter
//-----------------------------------------------------------------------------
IKimuraConverter* Kimura::CreateConverter(std::vector<std::string>& InArgs)
{
	// parse all the arguments into options
	ConverterOptions options;
	if (InArgs.size() <= 1 || !options.Parse(InArgs))
	{
		Converter::PrintHelp();
		return nullptr;
	}

	return new Converter(options);

}

//-----------------------------------------------------------------------------
// Converter::Start
//-----------------------------------------------------------------------------
void Converter::Start()
{
	this->MainWorkThread = new std::thread([this](){this->DoWorkFromMainWorkThread();});
}


//-----------------------------------------------------------------------------
// Converter::Stop
//-----------------------------------------------------------------------------
void Converter::Stop()
{
	if (this->MainWorkThread != nullptr)
	{
		this->Canceled = true;
		this->MainWorkThread->join();

		delete this->MainWorkThread;
	}
}


//-----------------------------------------------------------------------------
// Converter::DoWorkFromMainWorkThread
//-----------------------------------------------------------------------------
void Converter::DoWorkFromMainWorkThread()
{
	memset((void*)this->RaisedWarnings, 0, sizeof(this->RaisedWarnings));

	// validate access to alembic document
	if (!std::filesystem::exists(this->Options.SourceFile))
	{
		return this->FatalError("Couldn't find source file");
	}

	// load the archive
	this->AbcFactory.setPolicy(Alembic::Abc::ErrorHandler::kThrowPolicy);
	this->AbcFactory.setOgawaNumStreams(24);

	Alembic::AbcCoreFactory::IFactory::OgawaReadStrategy readStrategy = this->AbcFactory.getOgawaReadStrategy();

	Alembic::AbcCoreFactory::IFactory::CoreType CompressionType;
	this->AbcArchive = this->AbcFactory.getArchive(this->Options.SourceFile, CompressionType);
	if (!this->AbcArchive.valid())
	{
		return this->FatalError("Failed to load alembic archive from file");
	}

	// get the root object
	this->AbcRootObject = Alembic::Abc::IObject(this->AbcArchive, Alembic::Abc::kTop);
	if (!this->AbcRootObject.valid())
	{
		return this->FatalError("Archive has no root(top) object");
	}

	// determine Time Per Frame and frame rate
	Alembic::Abc::TimeSamplingPtr pTimeSampling = AbcArchive.getTimeSampling(AbcArchive.getNumTimeSamplings() > 1 ? 1 : 0);
	if (pTimeSampling)
	{
		this->TimePerFrame = (float)pTimeSampling->getTimeSamplingType().getTimePerCycle();
		this->FrameRate = 1.0f / this->TimePerFrame;
	}
	else
	{
		return this->FatalError("Couldn't find time sampling");
	}

	std::printf("Archive is valid. \n");
	std::printf("Framerate: %f. \n", this->FrameRate);

	this->TravelHierarchyToFindMeshes(this->AbcRootObject);

	std::printf("Number of meshes: %d \n", (int)this->Meshes.size());

	// go through all the meshes found and determine start and end frames
	for (const AbcArchiveMesh& m : this->Meshes)
	{
		// mesh's EndFrame is already clamped to the options' end frame
		if (m.EndFrame > this->EndFrame)
		{
			this->EndFrame = m.EndFrame;
		}
	}

	#ifdef SUPPORT_IMAGE_SEQUENCES
	{
		// Image sequences to process will only be discovered when SUPPORT_IMAGE_SEQUENCES is defined. 

		// discover all the image sequences that need converting.
		for (uint32 i = 0; i < this->Options.MaxImageSequences; i++)
		{
			if (!this->Options.ImageSequences[i].Path.empty())
			{
				this->DiscoverImageSequence(this->Options.ImageSequences[i]);
			}
		}

		std::printf("Number of image sequences: %d \n", (int)this->ImageSequences.size());

		// extend the end frame if any sequence of images 
		for (const InputImageSequence& imageSequence : this->ImageSequences)
		{
			if (imageSequence.Files.size() > this->EndFrame)
			{
				this->EndFrame = (int)imageSequence.Files.size();
			}
		}
	}
	#endif

	// options override start and end frame
	{
		this->StartFrame = this->Options.StartFrame;

		if (this->EndFrame > this->Options.EndFrame)
		{
			this->EndFrame = this->Options.EndFrame;
		}
	}

	std::printf("Start frame: %d \n", this->StartFrame);
	std::printf("End frame: %d \n", this->EndFrame);


	if (this->StartFrame > this->EndFrame)
	{
		return this->FatalError("Start frame higher than end frame. Nothing to convert");
	}
	
	// calculate number of frames
	this->NumFrames = this->EndFrame - this->StartFrame;

	std::printf("Num frames: %d \n", this->NumFrames);

	// set up table of content with placeholder data
	{
		this->TOC.SourceFile = this->Options.SourceFile;
		
		this->TOC.TimePerFrame = this->TimePerFrame;
		this->TOC.FrameRate = this->FrameRate;

		// meshes
		this->TOC.Meshes.resize(this->Meshes.size());
		for (uint32 iMesh=0; iMesh < this->Meshes.size(); iMesh++)
		{
			this->TOC.Meshes[iMesh].Name = this->Meshes[iMesh].Name;
			this->TOC.Meshes[iMesh].PositionFormat_ = this->Options.PositionFormat_;
			this->TOC.Meshes[iMesh].NormalFormat_ = this->Options.NormalFormat_;
			this->TOC.Meshes[iMesh].TangentFormat_ = this->Options.TangentFormat_;
			this->TOC.Meshes[iMesh].VelocityFormat_ = this->Options.VelocityFormat_;
			this->TOC.Meshes[iMesh].TexCoordFormat_ = this->Options.TexCoordFormat_;
			this->TOC.Meshes[iMesh].ColorFormat_ = this->Options.ColorFormat_;

		}

		// image sequences
		this->TOC.ImageSequences.resize(this->ImageSequences.size());
		for (uint32 i = 0; i < this->ImageSequences.size(); i++)
		{
			this->TOC.ImageSequences[i].Name = this->ImageSequences[i].Name;
			this->TOC.ImageSequences[i].Format = this->ImageSequences[i].Format;
			this->TOC.ImageSequences[i].Constant = (this->ImageSequences[i].Files.size() == 1) ? true : false;
		}

		// allocate frames along their meshes and image sequences
		this->TOC.Frames.resize(this->NumFrames);
		for (TOCFrame& f : this->TOC.Frames)
		{
			f.Meshes.resize(this->TOC.Meshes.size());
			f.Images.resize(this->TOC.ImageSequences.size());
		}

	}

	// process frames
	this->ProcessAndSaveAllTheFrames();

	// write the table of content at the beginning of the file
	if (!this->Canceled)
	{
		this->OutputFile.open(this->Options.DestinationFile, std::ios::out | std::ios::binary);
		if (!this->OutputFile.is_open())
		{
			this->FatalError("Failed to open the destination file");
			return;
		}

		// return to the beginning of the file
		this->OutputFile.seekp(0);

		// change the vertex element formats to 'None' when no data actually saved for the mesh
		{
			uint32 numMeshes = (uint32)this->TOC.Meshes.size();
			for (uint32 iMesh = 0; iMesh < numMeshes; iMesh++)
			{
				TOCMesh& m = this->TOC.Meshes[iMesh];

				if (!this->Meshes[iMesh].HasNormals)
				{
					if (this->Options.NormalFormat_ != NormalFormat::None)
					{
						this->RaiseWarning(Warnings::MissingNormals);
					}

					m.NormalFormat_ = NormalFormat::None;
				}

				if (!this->Meshes[iMesh].HasTangents)
				{
					if (this->Options.TangentFormat_ != TangentFormat::None)
					{
						this->RaiseWarning(Warnings::MissingTangents);
					}

					m.TangentFormat_ = TangentFormat::None;
				}

				if (!this->Meshes[iMesh].HasVelocity)
				{
					if (this->Options.VelocityFormat_ != VelocityFormat::None)
					{
						this->RaiseWarning(Warnings::MissingVelocity);
					}
					m.VelocityFormat_ = VelocityFormat::None;
				}

				if (!this->Meshes[iMesh].HasTexCoords)
				{
					if (this->Options.TexCoordFormat_ != TexCoordFormat::None)
					{
						this->RaiseWarning(Warnings::MissingTexCoords);
					}
					m.TexCoordFormat_ = TexCoordFormat::None;
				}

				if (!this->Meshes[iMesh].HasColors)
				{
					if (this->Options.ColorFormat_ != ColorFormat::None)
					{
						this->RaiseWarning(Warnings::MissingColors);
					}
					m.ColorFormat_ = ColorFormat::None;
				}

			}

		}

		// print warnings
		{
			bool bHasWarnings = false;
			for (int iWarning = 0; iWarning < (int)Warnings::Count; iWarning++)
			{
				if (this->RaisedWarnings[iWarning])
				{
					bHasWarnings = true;
					break;
				}
			}

			if (bHasWarnings)
			{
				std::printf("\n\nWarnings:\n");

				for (int iWarning = 0; iWarning < (int)Warnings::Count; iWarning++)
				{
					if (this->RaisedWarnings[iWarning])
					{
						const char* w = WarningMap[(Warnings)iWarning].c_str();
						std::printf("%s", w);
						std::printf("\n");
					}
				}

				std::printf("\n");
			}
		}

		std::printf("Writing table of content to output file...\n");
		this->WriteTableOfContent();

		// for each saved frame, append its data to the frame and update the TOC

		std::printf("Writing frames to output file...\n");

		for (uint32 iFrame=0; iFrame < this->TOC.Frames.size(); iFrame++)
		{
			std::ifstream	inputFile;
			std::string frameFilename = "./cache/" + std::to_string(iFrame);

			if (!std::filesystem::exists(frameFilename))
			{
				this->FatalError("Failed to open cached file for reading");
				return;
			}

			// get the file's size
			uint64 fileSize = std::filesystem::file_size(frameFilename);

			inputFile.open(frameFilename, std::ios::in | std::ios::binary);
			if (!inputFile.is_open())
			{
				this->FatalError("Failed to open cached file for reading");
				return;
			}

			// read entire content of cached file into memory
			std::vector<uint8> fileBuffer(fileSize);
			inputFile.read((char*)fileBuffer.data(), fileSize);

			// append cached file data to output file
			this->Write(fileBuffer);

			// done with input file
			inputFile.close();

			// we're done with this file, we can delete it
			std::filesystem::remove(frameFilename);

			std::printf(".");

		}

		std::printf("\n\nFile written: %s\n", this->Options.DestinationFile.c_str());
		
		this->OutputFile.close();

	}

	this->Done = true;

}


//-----------------------------------------------------------------------------
// Converter::PrintHelp
//-----------------------------------------------------------------------------
void Converter::PrintHelp()
{
	std::printf("\nKimura Converter Tool, version %s\n", Version().ToString().c_str());
	std::printf("by Alexandre Hetu (alex@kimuraplayer.com).\n");
	std::printf("Syntax: abcToKimura.exe i:<input file> o:<output file> option:<...>\n");

	std::printf("\nOptions:\n");
	std::printf("   scale: Scales the geometry by this value. Default is 1.0. \n");
	std::printf("   start: Frame at which the converter will start converting frames. Default is 0.\n");
	std::printf("   end: Frame at which the converter will stop converting frames. Default is 9999999.\n");
	std::printf("   split: Split and optimize meshes to force use of 16bit index buffers. Default is 'true'.\n");
	std::printf("   pFmt: Format in which positions are saved. Can be 'full', 'half'. Default is 'full'.\n");
	std::printf("   nFmt: Format in which normals (if present) are saved. Can be 'full', 'half', 'byte' and 'none'. Default is 'half'.\n");
	std::printf("   ntFmt: Format in which normals tangent are saved. Requires normals and texture coordinates. Can be 'full', 'half', 'byte' and 'none'. Default is 'half'.\n");
	std::printf("   vFmt: Format in which velocities (if present) are saved. Can be 'full', 'half', 'byte', and 'none'. Default is 'byte'.\n");
	std::printf("   tFmt: Format in which texture coordinates (if present) are saved. Can be 'full', 'half' and 'none'. Default is 'half'.\n");
	std::printf("   cFmt: Format in which color channels (if present) are saved. Can be 'full', 'half', 'byte', 'bytehdr' and 'none'. Default is 'byte'.\n");
	std::printf("   swizzle: Swap axises. Can be 'yz', 'xz' and 'none'. Default is 'none'.\n");
	std::printf("   flip: Flip order of triangle indices. Default is 'false'.\n");
	std::printf("   flipUV: Flip texture coordinates along V. Default is 'true'.\n");
	std::printf("   cpu: Number of threads used for processing frames. By default, this is automatically set to the number of cores available. \n");

	
	std::printf("   image[index]: Path to a file image, or the first file image of a sequence.\n");
	std::printf("   image[index]max: Max size of converted images for image sequence.\n");
	std::printf("   image[index]mips: Whether to generate mipmaps for image sequence.\n");
	std::printf("   image[index]fmt: Texture format to convert to. Available formats are:\n");
	std::printf("                    DXT1, DXT3, DXT5\n");

	std::printf("\n");
	std::printf("ex: abcToKimura.exe i:\"c:/alembicFile.abc\" o:\"kimuraFile.k\"\n");
	std::printf("ex: abcToKimura.exe i:\"c:/alembicFile.abc\" o:./kimuraFile.k start:10 end:200 scale:10.0\n");
	std::printf("ex: abcToKimura.exe i:\"c:/alembicFile.abc\" o:./kimuraFile.k image0:./Images/image.00001.png\n");
	std::printf("ex: abcToKimura.exe i:\"c:/alembicFile.abc\" o:./kimuraFile.k image0:./Images/image.00001.png image0fmt:DXT1\n");

}


//-----------------------------------------------------------------------------
// Converter::FatalError
//-----------------------------------------------------------------------------
void Kimura::Converter::FatalError(std::string InErrorMessage)
{
	this->Error = true;
	this->ErrorMessage = InErrorMessage;
}


namespace Kimura
{
	//-----------------------------------------------------------------------------
	// Kimura::Dot
	//-----------------------------------------------------------------------------
	inline float Dot(const Vector3 & a, const Vector3 & b)
	{
		return (a.X * b.X) + (a.Y * b.Y) + (a.Z * b.Z);
	}

	//-----------------------------------------------------------------------------
	// Kimura::Cross
	//-----------------------------------------------------------------------------
	inline Vector3 Cross(const Vector3 & a, const Vector3 & b)
	{
		float x = a.Y * b.Z - b.Y * a.Z;
		float y = a.Z * b.X - b.Z * a.X;
		float z = a.X * b.Y - b.X * a.Y;

		return Vector3(x, y, z);
	}

	//-----------------------------------------------------------------------------
	// Kimura::Normalize
	//-----------------------------------------------------------------------------
	inline Vector3 Normalize(const Vector3 & a)
	{
		float fLength = (float)sqrt((a.X * a.X) + (a.Y * a.Y) + (a.Z * a.Z));

		return Vector3(a.X / fLength, a.Y / fLength, a.Z / fLength);
	}

}


//-----------------------------------------------------------------------------
// Converter::GenerateTangentsOnFrameMesh
//-----------------------------------------------------------------------------
void Converter::GenerateTangentsOnFrameMesh(FrameMeshData& InOutMeshData)
{
	// assuming the mesh has already been optimized and degenerate triangles have been discarded

	std::vector<Vector3>	tan1;
	std::vector<Vector3>	tan2;

	tan1.resize(InOutMeshData.Positions.size());
	tan2.resize(InOutMeshData.Positions.size());

	for (uint32 iSurface = 0; iSurface < InOutMeshData.Surfaces; iSurface++)
	{

		uint32 i1 = InOutMeshData.Indices[iSurface * 3 + 0];
		uint32 i2 = InOutMeshData.Indices[iSurface * 3 + 1];
		uint32 i3 = InOutMeshData.Indices[iSurface * 3 + 2];

		if (this->Options.FlipIndiceOrder)
		{
			uint32 tmp = i3;
			i3 = i2;
			i2 = tmp;
		}


		Vector3 v1 = InOutMeshData.Positions[i1];
		Vector3 v2 = InOutMeshData.Positions[i2];
		Vector3 v3 = InOutMeshData.Positions[i3];

		// perform swizzling here, before mixing with texture coords
		if (this->Options.Swizzle_ == Swizzle::XZ)
		{
			v1.SwizzleXZ();
			v2.SwizzleXZ();
			v3.SwizzleXZ();
		}
		else if (this->Options.Swizzle_ == Swizzle::YZ)
		{
			v1.SwizzleYZ();
			v2.SwizzleYZ();
			v3.SwizzleYZ();
		}


		Vector2 w1 = InOutMeshData.UVChannels[0][i1];
		Vector2 w2 = InOutMeshData.UVChannels[0][i2];
		Vector2 w3 = InOutMeshData.UVChannels[0][i3];

		if (this->Options.FlipTextureCoords)
		{
			w1.Y = 1.0f - w1.Y;
			w2.Y = 1.0f - w2.Y;
			w3.Y = 1.0f - w3.Y;
		}

		float x1 = v2.X - v1.X;
		float x2 = v3.X - v1.X;
		float y1 = v2.Y - v1.Y;
		float y2 = v3.Y - v1.Y;
		float z1 = v2.Z - v1.Z;
		float z2 = v3.Z - v1.Z;

		float s1 = w2.X - w1.X;
		float s2 = w3.X - w1.X;
		float t1 = w2.Y - w1.Y;
		float t2 = w3.Y - w1.Y;

		float r = 1.0f / (s1 * t2 - s2 * t1);

		if (isinf(r))
		{
			r = 0.0f;
		}

		Vector3 sdir(	(t2 * x1 - t1 * x2) * r, 
						(t2 * y1 - t1 * y2) * r,
						(t2 * z1 - t1 * z2) * r);
		Vector3 tdir(	(s1 * x2 - s2 * x1) * r, 
						(s1 * y2 - s2 * y1) * r,
						(s1 * z2 - s2 * z1) * r);

		tan1[i1] += sdir;
 		tan1[i2] += sdir;
 		tan1[i3] += sdir;
 
 		tan2[i1] += tdir;
 		tan2[i2] += tdir;
 		tan2[i3] += tdir;

	}

	InOutMeshData.Tangents.resize(InOutMeshData.Positions.size());
	for (uint64 i = 0; i < InOutMeshData.Positions.size(); i++)
	{
		const Vector3& n = InOutMeshData.Normals[i];
		const Vector3 t = Normalize(tan1[i]);

		// Gram-Schmidt orthogonalized
		Vector3 v = Normalize(t - n * Dot(n, t));

		InOutMeshData.Tangents[i].X = v.X;
		InOutMeshData.Tangents[i].Y = v.Y;
		InOutMeshData.Tangents[i].Z = v.Z;


		const Vector3 t2 = Normalize(tan2[i]);

		// Calculate handedness
		InOutMeshData.Tangents[i].W = (Dot(Cross(n, t), t2) < 0.0f) ? -1.0f : 1.0f;;

	}

}


template<typename T>
uint32 Converter::Write(T& In, uint32 InCount /*=1*/)
{
	uint64 posBefore = this->OutputFile.tellp();
	this->OutputFile.write((const char*)&In, sizeof(T) * InCount);
	uint64 posAfter = this->OutputFile.tellp();

	return (uint32)(posAfter - posBefore);
}

uint32 Converter::Write(std::string& s)
{
	uint64 posBefore = this->OutputFile.tellp();
	
	int size = (int)s.length();
	this->OutputFile.write((const char*)&size, sizeof(size));
	if (size > 0)
	{
		this->OutputFile.write(s.c_str(), sizeof(char) * size);
	}
	uint64 posAfter = this->OutputFile.tellp();

	return (uint32)(posAfter - posBefore);
}

uint32 Converter::Write(std::vector<byte>& d)
{
	if (d.empty())
	{
		return 0;
	}

	uint64 posBefore = this->OutputFile.tellp();
	this->OutputFile.write((const char*)d.data(), d.size());
	uint64 posAfter = this->OutputFile.tellp();

	return (uint32)(posAfter - posBefore);
}


//-----------------------------------------------------------------------------
// Converter::WriteTableOfContent
//-----------------------------------------------------------------------------
void Converter::WriteTableOfContent()
{

	this->Write<Version>(this->TOC.Version_);
	this->Write(this->TOC.SourceFile);
	this->Write(this->TOC.CreationDate);

	this->Write<float>(this->TOC.TimePerFrame);
	this->Write<float>(this->TOC.FrameRate);

	uint32 b16BitIndices = this->Options.Force16bitIndices ? 1 : 0;
	this->Write<uint32>(b16BitIndices);

	// meshes
	{
		uint32 numMeshes = (uint32)this->TOC.Meshes.size();
		this->Write<uint32>(numMeshes);

		for (uint32 iMesh = 0; iMesh < numMeshes; iMesh++)
		{
			TOCMesh& m = this->TOC.Meshes[iMesh];

			this->Write(m.Name);

			this->Write<bool>(m.Constant);
			this->Write<uint64>(m.MaxVertices);
			this->Write<uint64>(m.MaxSurfaces);
			this->Write<PositionFormat>(m.PositionFormat_);
			this->Write<NormalFormat>(m.NormalFormat_);
			this->Write<TangentFormat>(m.TangentFormat_);
			this->Write<VelocityFormat>(m.VelocityFormat_);
			this->Write<TexCoordFormat>(m.TexCoordFormat_);
			this->Write<ColorFormat>(m.ColorFormat_);
		}
	}

	// image sequences
	{
		uint32 numImageSequences = (uint32)this->TOC.ImageSequences.size();
		this->Write<uint32>(numImageSequences);
		for (uint32 iIS = 0; iIS < numImageSequences; iIS++)
		{
			TOCImageSequence& IS = this->TOC.ImageSequences[iIS];

			this->Write(IS.Name);
			this->Write<ImageFormat>(IS.Format);

			this->Write<bool>(IS.Constant);
			this->Write<uint32>(IS.Width);
			this->Write<uint32>(IS.Height);
			this->Write<uint32>(IS.MipMapCount);

		}

	}

	// frames
	{
		uint32 numFrames = (uint32) this->TOC.Frames.size();
		this->Write<uint32>(numFrames);

		for (uint32 iFrame = 0; iFrame < numFrames; iFrame++)
		{
			TOCFrame& f = this->TOC.Frames[iFrame];

			this->Write<uint64>(f.FilePosition);
			this->Write<uint64>(f.BufferSize);

			for (uint32 iMesh = 0; iMesh < this->TOC.Meshes.size(); iMesh++)
			{
				TOCFrameMesh& fm = f.Meshes[iMesh];

				this->Write<uint32>(fm.Vertices);
				this->Write<uint32>(fm.Surfaces);

				uint32 numSections = (uint32)fm.Sections.size();
				this->Write<uint32>(numSections);
				for (TOCFrameMeshSection& s : fm.Sections)
				{
					this->Write<uint32>(s.VertexStart);
					this->Write<uint32>(s.IndexStart);
					this->Write<uint32>(s.NumSurfaces);
					this->Write<uint32>(s.MinVertexIndex);
					this->Write<uint32>(s.MaxVertexIndex);
				}

				this->Write<int32>(fm.SeekIndices);
				this->Write<uint32>(fm.SizeIndices);

				this->Write<int32>(fm.SeekPositions);
				this->Write<uint32>(fm.SizePositions);
				this->Write<Kimura::Vector3>(fm.PositionQuantizationCenter);
				this->Write<Kimura::Vector3>(fm.PositionQuantizationExtents);

				this->Write<int32>(fm.SeekNormals);
				this->Write<uint32>(fm.SizeNormals);

				this->Write<int32>(fm.SeekTangents);
				this->Write<uint32>(fm.SizeTangents);

				this->Write<int32>(fm.SeekVelocities);
				this->Write<uint32>(fm.SizeVelocities);
				this->Write<Kimura::Vector3>(fm.VelocityQuantizationCenter);
				this->Write<Kimura::Vector3>(fm.VelocityQuantizationExtents);

				this->Write<int32>(fm.SeekTexCoords[0], MaxTextureCoords);
				this->Write<uint32>(fm.SizeTexCoords[0], MaxTextureCoords);

				this->Write<int32>(fm.SeekColors[0], MaxColorChannels);
				this->Write<uint32>(fm.SizeColors[0], MaxColorChannels);
				this->Write<Kimura::Vector4>(fm.ColorQuantizationExtents[0], MaxColorChannels);

				this->Write<Kimura::Vector3>(fm.BoundingCenter);
				this->Write<Kimura::Vector3>(fm.BoundingSize);

			}

			for (uint32 iIS = 0; iIS  < this->TOC.ImageSequences.size(); iIS++)
			{
				TOCFrameImage& fi = f.Images[iIS];

				this->Write<uint32>(fi.NumMipmaps);
				for (uint32 iMipmap = 0; iMipmap < MaxMipmaps; iMipmap++)
				{
					this->Write<uint32>(fi.Mipmaps[iMipmap].Width);
					this->Write<uint32>(fi.Mipmaps[iMipmap].Height);
					this->Write<uint32>(fi.Mipmaps[iMipmap].RowPitch);
					this->Write<uint32>(fi.Mipmaps[iMipmap].SlicePitch);

					this->Write<int32>(fi.Mipmaps[iMipmap].SeekPosition);
					this->Write<uint32>(fi.Mipmaps[iMipmap].Size);

				}

			}

		}

	}

}


//-----------------------------------------------------------------------------
// Converter::ProcessAndSaveFrames
//-----------------------------------------------------------------------------
void Converter::ProcessAndSaveAllTheFrames()
{
	// unless explicitly specified, use about half the cores for the number of thread workers. 
	int numWorkers = this->Options.NumThreadUsedForProcessingFrames != -1 ? this->Options.NumThreadUsedForProcessingFrames : std::thread::hardware_concurrency() / 2;
	if (numWorkers < 1)
	{
		numWorkers = 1;
	}
	else if (numWorkers > 64)
	{
		numWorkers = 64;
	}

	// in debug, always use a single thread to simplify debugging
	#if _DEBUG
		numWorkers = 1;
	#endif

	std::printf("Processing on %d thread(s)\n", numWorkers);

	// initialize the thread pool
	Threadpool* frameProcessingPool = new Threadpool("Frame processing", numWorkers);

	// initialize an array capable of receiving all of the completed frames
	this->Frames.resize(this->NumFrames);

	int maxTasksToQueue = numWorkers > 16 ? numWorkers : 16;

	while ((this->IndexOfNextFrameToWrite) < this->NumFrames && !this->Canceled)
	{

		class TaskProcessFrame : public IThreadPoolTask
		{
		public:

			TaskProcessFrame(Converter* InConverter, int InFrameSaveIndex, int InFrameProcessIndex)
			{
				this->Converter_ = InConverter;
				this->FrameSaveIndex = InFrameSaveIndex;
				this->FrameProcessIndex = InFrameProcessIndex;
			}

			virtual void Execute() override
			{
				this->Converter_->ProcessFrame(this->FrameSaveIndex, this->FrameProcessIndex);
			}

			Converter* Converter_ = nullptr;
			int FrameSaveIndex;
			int FrameProcessIndex;
		};


		// queue more work and peek at the next frame to save
		std::shared_ptr<Frame> nextFrameToSave = nullptr;
		{
			std::unique_lock<std::mutex> threadLock(this->FrameProcessingMutex);

			// if there more work that need to be queued? Queue as a little as 16 tasks, up to the number of workers available if possible
			while (	this->IndexOfLastFrameQueuedForProcessing < this->NumFrames &&
					this->IndexOfNextFrameToWrite + maxTasksToQueue > this->IndexOfLastFrameQueuedForProcessing)
			{
				// add new tasks for 
				std::unique_ptr<TaskProcessFrame> task = std::make_unique<TaskProcessFrame>(this, 
																							this->IndexOfLastFrameQueuedForProcessing, 
																							this->StartFrame + this->IndexOfLastFrameQueuedForProcessing);
				frameProcessingPool->AddTask(std::move(task));

				this->IndexOfLastFrameQueuedForProcessing++;
			}

			// is the next frame ready to be saved?
			if (this->Frames[this->IndexOfNextFrameToWrite] != nullptr)
			{
				nextFrameToSave = this->Frames[this->IndexOfNextFrameToWrite];
				this->Frames[this->IndexOfNextFrameToWrite] = nullptr;
				this->IndexOfNextFrameToWrite++;
			}
		}

		// is the next frame ready?
		if (nextFrameToSave != nullptr)
		{
			this->UpdateTOCAndWriteFrameToDisk(nextFrameToSave);
		}
		else
		{
			// give the thread workers the time to complete some work
			std::this_thread::sleep_for(100ms);
		}

	}

	if (this->Canceled)
	{
		frameProcessingPool->Stop();
	}

	// done with this pool. There should be no more work in its queue
	delete frameProcessingPool;
	frameProcessingPool = nullptr;

}


//-----------------------------------------------------------------------------
// Converter::UpdateTOCAndWriteFrameToDisk
//-----------------------------------------------------------------------------
void Converter::UpdateTOCAndWriteFrameToDisk(std::shared_ptr<Frame> InFrameToSave)
{
	
	TOCFrame& tocFrame = this->TOC.Frames[InFrameToSave->FrameIndex];

	// FilePosition is relative to the start of the first frame
	tocFrame.FilePosition = this->CurrentFrameOffset;

	{
		
		if (!std::filesystem::exists("./cache"))
		{
			std::filesystem::create_directory("./cache");
		}

		std::string frameFilename = "./cache/" + std::to_string(InFrameToSave->FrameIndex);
		
		this->OutputFile.open(frameFilename, std::ios::out | std::ios::binary);
		if (!this->OutputFile.is_open())
		{
			this->FatalError("Failed to open cache file for writing");
			return;
		}

	}


	uint32 pos = 0;
	for (uint32 iMesh = 0; iMesh < InFrameToSave->Meshes.size(); iMesh++)
	{
		// save the mesh's info
		tocFrame.Meshes[iMesh].Vertices = (uint32)InFrameToSave->Meshes[iMesh].Positions.size();
		tocFrame.Meshes[iMesh].Surfaces = InFrameToSave->Meshes[iMesh].Surfaces;
		tocFrame.Meshes[iMesh].BoundingCenter = InFrameToSave->Meshes[iMesh].BoundingCenter;
		tocFrame.Meshes[iMesh].BoundingSize = InFrameToSave->Meshes[iMesh].BoundingSize;
		tocFrame.Meshes[iMesh].PositionQuantizationCenter = InFrameToSave->Meshes[iMesh].PositionQuantizationCenter;
		tocFrame.Meshes[iMesh].PositionQuantizationExtents = InFrameToSave->Meshes[iMesh].PositionQuantizationExtents;
		tocFrame.Meshes[iMesh].VelocityQuantizationCenter = InFrameToSave->Meshes[iMesh].VelocityQuantizationCenter;
		tocFrame.Meshes[iMesh].VelocityQuantizationExtents = InFrameToSave->Meshes[iMesh].VelocityQuantizationExtents;

		// save the mesh's sections
		{

			for (FrameMeshData::Section& s : InFrameToSave->Meshes[iMesh].Sections)
			{

				TOCFrameMeshSection s2;
				s2.VertexStart = s.VertexStart;
				s2.IndexStart = s.IndexStart;
				s2.NumSurfaces = s.NumSurfaces;
				s2.MinVertexIndex = s.MinVertexIndex;
				s2.MaxVertexIndex = s.MaxVertexIndex;

				tocFrame.Meshes[iMesh].Sections.push_back(s2);
			}

		}



		for (int iColor=0; iColor < MaxColorChannels; iColor++)
		{
			tocFrame.Meshes[iMesh].ColorQuantizationExtents[iColor] = InFrameToSave->Meshes[iMesh].ColorQuantizationExtents[iColor];
		}

		// keep track of the maximum number of vertices and surfaces necessary for this mesh
		{
			TOCMesh& tocMesh = this->TOC.Meshes[iMesh];

			if (tocFrame.Meshes[iMesh].Vertices > tocMesh.MaxVertices)
			{
				tocMesh.MaxVertices = tocFrame.Meshes[iMesh].Vertices;
			}

			if (tocFrame.Meshes[iMesh].Surfaces > tocMesh.MaxSurfaces)
			{
				tocMesh.MaxSurfaces = tocFrame.Meshes[iMesh].Surfaces;
			}

			if (InFrameToSave->Meshes[iMesh].Normals.size() > 0)
			{
				this->Meshes[iMesh].HasNormals = true;
			}

			if (InFrameToSave->Meshes[iMesh].Tangents.size() > 0)
			{
				this->Meshes[iMesh].HasTangents = true;
			}

			if (InFrameToSave->Meshes[iMesh].Velocities.size() > 0)
			{
				this->Meshes[iMesh].HasVelocity = true;
			}

			if (InFrameToSave->Meshes[iMesh].UVCount > 0)
			{
				this->Meshes[iMesh].HasTexCoords = true;
			}

			if (InFrameToSave->Meshes[iMesh].ColorCount > 0)
			{
				this->Meshes[iMesh].HasColors = true;
			}

		}


		// indices
		if (this->LastFrameSaved != nullptr &&
			InFrameToSave->Meshes[iMesh].IndicesPacked.size() > 0 &&
			this->LastFrameSaved->Meshes[iMesh].IndicesHash == InFrameToSave->Meshes[iMesh].IndicesHash)
		{
			tocFrame.Meshes[iMesh].SeekIndices = -1;
		}
		else
		{
			tocFrame.Meshes[iMesh].SeekIndices = pos;
			tocFrame.Meshes[iMesh].SizeIndices = (uint32) InFrameToSave->Meshes[iMesh].IndicesPacked.size();

			pos += this->Write(InFrameToSave->Meshes[iMesh].IndicesPacked);
		}

		// positions
		if (this->LastFrameSaved != nullptr &&
			InFrameToSave->Meshes[iMesh].PositionsPacked.size() > 0 &&
			this->LastFrameSaved->Meshes[iMesh].PositionsHash == InFrameToSave->Meshes[iMesh].PositionsHash)
		{
			tocFrame.Meshes[iMesh].SeekPositions = -1;
		}
		else
		{
			tocFrame.Meshes[iMesh].SeekPositions = pos;
			tocFrame.Meshes[iMesh].SizePositions = (uint32) InFrameToSave->Meshes[iMesh].PositionsPacked.size();

			pos += this->Write(InFrameToSave->Meshes[iMesh].PositionsPacked);
		}

		// normals
		if (this->LastFrameSaved != nullptr &&
			InFrameToSave->Meshes[iMesh].NormalsPacked.size() > 0 &&
			this->LastFrameSaved->Meshes[iMesh].NormalsHash == InFrameToSave->Meshes[iMesh].NormalsHash)
		{
			tocFrame.Meshes[iMesh].SeekNormals = -1;
		}
		else
		{
			tocFrame.Meshes[iMesh].SeekNormals = pos;
			tocFrame.Meshes[iMesh].SizeNormals = (uint32)InFrameToSave->Meshes[iMesh].NormalsPacked.size();

			pos += this->Write(InFrameToSave->Meshes[iMesh].NormalsPacked);
		}

		// tangents
		if (this->LastFrameSaved != nullptr &&
			InFrameToSave->Meshes[iMesh].TangentsPacked.size() > 0 &&
			this->LastFrameSaved->Meshes[iMesh].TangentsHash == InFrameToSave->Meshes[iMesh].TangentsHash)
		{
			tocFrame.Meshes[iMesh].SeekTangents = -1;
		}
		else
		{
			tocFrame.Meshes[iMesh].SeekTangents = pos;
			tocFrame.Meshes[iMesh].SizeTangents = (uint32)InFrameToSave->Meshes[iMesh].TangentsPacked.size();

			pos += this->Write(InFrameToSave->Meshes[iMesh].TangentsPacked);
		}


		// velocities
		if (this->LastFrameSaved != nullptr &&
			InFrameToSave->Meshes[iMesh].VelocitiesPacked.size() > 0 &&
			this->LastFrameSaved->Meshes[iMesh].VelocitiesHash == InFrameToSave->Meshes[iMesh].VelocitiesHash)
		{
			tocFrame.Meshes[iMesh].SeekVelocities = -1;
		}
		else
		{
			tocFrame.Meshes[iMesh].SeekVelocities = pos;
			tocFrame.Meshes[iMesh].SizeVelocities = (uint32)InFrameToSave->Meshes[iMesh].VelocitiesPacked.size();

			pos += this->Write(InFrameToSave->Meshes[iMesh].VelocitiesPacked);
		}

		// texcoords
		for (uint32 iTC = 0; iTC < (uint32)InFrameToSave->Meshes[iMesh].UVCount; iTC++)
		{
			if (this->LastFrameSaved != nullptr &&
				InFrameToSave->Meshes[iMesh].UVChannelsPacked[iTC].size() > 0 &&
				this->LastFrameSaved->Meshes[iMesh].UVChannelsHash[iTC] == InFrameToSave->Meshes[iMesh].UVChannelsHash[iTC])
			{
				tocFrame.Meshes[iMesh].SeekTexCoords[iTC] = -1;
			}
			else
			{
				tocFrame.Meshes[iMesh].SeekTexCoords[iTC] = pos;
				tocFrame.Meshes[iMesh].SizeTexCoords[iTC] = (uint32)InFrameToSave->Meshes[iMesh].UVChannelsPacked[iTC].size();

				pos += this->Write(InFrameToSave->Meshes[iMesh].UVChannelsPacked[iTC]);
			}
		}

		// colors
		for (uint32 iColor = 0; iColor < (uint32)InFrameToSave->Meshes[iMesh].ColorCount; iColor++)
		{
			if (this->LastFrameSaved != nullptr &&
				InFrameToSave->Meshes[iMesh].ColorsPacked[iColor].size() > 0 &&
				this->LastFrameSaved->Meshes[iMesh].ColorsHash[iColor] == InFrameToSave->Meshes[iMesh].ColorsHash[iColor])
			{
				tocFrame.Meshes[iMesh].SeekColors[iColor] = -1;
			}
			else
			{
				tocFrame.Meshes[iMesh].SeekColors[iColor] = pos;
				tocFrame.Meshes[iMesh].SizeColors[iColor] = (uint32)InFrameToSave->Meshes[iMesh].ColorsPacked[iColor].size();

				pos += this->Write(InFrameToSave->Meshes[iMesh].ColorsPacked[iColor]);
			}
		}

	}

	uint32 bytesUsedOnMeshes = pos;

	// write all the images for this frame
	for (uint32 iIS = 0; iIS < InFrameToSave->Images.size(); iIS++)
	{
		FrameImageData& frameImageData = InFrameToSave->Images[iIS];
		TOCFrameImage& tocFrameImage = tocFrame.Images[iIS];

		// first frame provides info about the entire sequence
		if (InFrameToSave->FrameIndex == 0)
		{
			this->TOC.ImageSequences[iIS].MipMapCount = frameImageData.NumMipmaps;
			this->TOC.ImageSequences[iIS].Width = frameImageData.Mipmaps[0].Width;
			this->TOC.ImageSequences[iIS].Height = frameImageData.Mipmaps[0].Height;
		}

		tocFrameImage.NumMipmaps = frameImageData.NumMipmaps;
		for (int iMipmap = 0; iMipmap < MaxMipmaps; iMipmap++)
		{
			// if mipmap is the same as the previous frame
			if (LastFrameSaved != nullptr &&
				LastFrameSaved->Images[iIS].Mipmaps[iMipmap].Data.size() > 0 &&
				LastFrameSaved->Images[iIS].Mipmaps[iMipmap].DataHash == InFrameToSave->Images[iIS].Mipmaps[iMipmap].DataHash)
			{
				tocFrameImage.Mipmaps[iMipmap].SeekPosition = -1;
			}
			else if (iMipmap >= frameImageData.NumMipmaps)
			{
				tocFrameImage.Mipmaps[iMipmap].SeekPosition = -1;
			}
			else
			{
				tocFrameImage.Mipmaps[iMipmap].Width = InFrameToSave->Images[iIS].Mipmaps[iMipmap].Width;
				tocFrameImage.Mipmaps[iMipmap].Height = InFrameToSave->Images[iIS].Mipmaps[iMipmap].Height;
				tocFrameImage.Mipmaps[iMipmap].RowPitch = InFrameToSave->Images[iIS].Mipmaps[iMipmap].RowPitch;
				tocFrameImage.Mipmaps[iMipmap].SlicePitch = InFrameToSave->Images[iIS].Mipmaps[iMipmap].SlicePitch;

				tocFrameImage.Mipmaps[iMipmap].SeekPosition = pos;
				tocFrameImage.Mipmaps[iMipmap].Size = (uint32)InFrameToSave->Images[iIS].Mipmaps[iMipmap].Data.size();

				pos += this->Write(InFrameToSave->Images[iIS].Mipmaps[iMipmap].Data);
			}

		}

	}

	uint32 bytesUsedOnImages = pos - bytesUsedOnMeshes;

	// total size of the frame's buffer
	tocFrame.BufferSize = pos;

	// move frame offset 
	this->CurrentFrameOffset += tocFrame.BufferSize;

	if (this->Options.Verbose)
	{
		std::printf("Frame %d. Total size = %d, Mesh = %d bytes, Image data = %d bytes\n", InFrameToSave->FrameIndex, pos, bytesUsedOnMeshes, bytesUsedOnImages);

		for (int i=0; i<InFrameToSave->Meshes.size(); i++)
		{
			const auto& m = InFrameToSave->Meshes[i];

			int pSize = (int)m.PositionsPacked.size();
			int nSize = (int)m.NormalsPacked.size();
			int ntSize = (int)m.TangentsPacked.size();
			int vSize = (int)m.VelocitiesPacked.size();
			int uvSize = (int)m.UVChannelsPacked[0].size();
			int cSize = (int)m.ColorsPacked[0].size();

			int indiceSize = (int)m.IndicesPacked.size();

			int total = pSize + nSize + ntSize + vSize + uvSize + cSize + indiceSize;

			std::printf("    Mesh %s: surfaces=%d, vertices=%d, p=%d, n=%d, nt=%d, v=%d, t=%d, c=%d, indices=%d, total=%d\n", this->Meshes[i].Name.c_str(), m.Surfaces, (int)m.Positions.size(), pSize, nSize, ntSize, vSize, uvSize, cSize, indiceSize, total);

		}

		for (int i = 0; i < InFrameToSave->Images.size(); i++)
		{
			const auto& image = InFrameToSave->Images[i];

			int imageSize = 0;
			for (int m=0; m<image.NumMipmaps; m++)
			{
				imageSize += (int)image.Mipmaps[m].Data.size();
			}

			std::printf("    Image %s: mipmaps=%d, size=%d\n", this->ImageSequences[i].Name.c_str(), image.NumMipmaps, imageSize);
		}
	}

	this->OutputFile.close();

	this->LastFrameSaved = InFrameToSave;
	this->NumFramesSaved++;
}


//-----------------------------------------------------------------------------
// Converter::TravelHierarchyToFindMeshes
//-----------------------------------------------------------------------------
void Converter::TravelHierarchyToFindMeshes(Alembic::Abc::IObject& InObject)
{
	const Alembic::Abc::MetaData metaData = InObject.getMetaData();


	// we support either subD or poly meshes
	if (Alembic::AbcGeom::ISubD::matches(metaData))
	{
		this->AddMeshFromISubD(InObject);
	}
	else if (Alembic::AbcGeom::IPolyMesh::matches(metaData))
	{
		this->AddMeshFromIPolyMesh(InObject);
	}

	// recursively traverse the hierarchy to find more meshes
	for (int iChild = 0; iChild < InObject.getNumChildren(); iChild++)
	{
		Alembic::Abc::IObject c = InObject.getChild(iChild);
		this->TravelHierarchyToFindMeshes(c);
	}
}


//-----------------------------------------------------------------------------
// Converter::AddMeshFromIPolyMesh
//-----------------------------------------------------------------------------
void Converter::AddMeshFromIPolyMesh(Alembic::Abc::IObject& InObject)
{
	AbcArchiveMesh newMesh;
	{
		// save ref to IObject and its name
		newMesh.AbcObject = InObject;
		newMesh.Name = InObject.getName();

		Alembic::AbcGeom::IPolyMesh polymesh = Alembic::AbcGeom::IPolyMesh(InObject, Alembic::Abc::kWrapExisting);
		const Alembic::AbcGeom::IPolyMeshSchema schema(polymesh.getSchema());

		// find start and end times
		Alembic::AbcCoreAbstract::TimeSamplingPtr timeSampling = schema.getTimeSampling();
		newMesh.StartFrame = (int)(timeSampling->getSampleTime(0) / this->TimePerFrame);
		newMesh.EndFrame = (int)(timeSampling->getSampleTime(schema.getNumSamples() - 1) / this->TimePerFrame);

	}

	this->Meshes.push_back(newMesh);
}


//-----------------------------------------------------------------------------
// Converter::AddMeshFromISubD
//-----------------------------------------------------------------------------
void Converter::AddMeshFromISubD(Alembic::Abc::IObject& InObject)
{
	AbcArchiveMesh newMesh;
	{
		// save ref to IObject and its name
		newMesh.AbcObject = InObject;
		newMesh.Name = InObject.getName();

		Alembic::AbcGeom::ISubD subDMesh = Alembic::AbcGeom::ISubD(InObject, Alembic::Abc::kWrapExisting);
		const Alembic::AbcGeom::ISubDSchema schema(subDMesh.getSchema());

		// find start and end times
		Alembic::AbcCoreAbstract::TimeSamplingPtr timeSampling = schema.getTimeSampling();
		newMesh.StartFrame = (int)(timeSampling->getSampleTime(0) / this->TimePerFrame);
		newMesh.EndFrame = (int)(timeSampling->getSampleTime(schema.getNumSamples() - 1) / this->TimePerFrame);

	}

	this->Meshes.push_back(newMesh);
}


//-----------------------------------------------------------------------------
// Converter::DiscoverImageSequence
//-----------------------------------------------------------------------------
void Converter::DiscoverImageSequence(ConverterOptions::ImageSequenceOptions& InImageSequenceOptions)
{
	// does the file event exist
	if (!std::filesystem::exists(InImageSequenceOptions.Path))
	{
		std::printf("ERROR: Invalid image sequence path: '%s'", InImageSequenceOptions.Path.c_str());
		return;
	}

	std::filesystem::path p = InImageSequenceOptions.Path;
	std::filesystem::path basePath = std::filesystem::absolute(p);
	
	std::string filenameOnly = basePath.filename().string();
	filenameOnly = filenameOnly.substr(0, filenameOnly.find('.'));

	basePath = basePath.parent_path();

	InputImageSequence newImageSequence;
	newImageSequence.Name = filenameOnly;
	newImageSequence.Path = basePath.string();
	newImageSequence.Format = ImageFormat::DXT1;
	newImageSequence.Mipmaps = InImageSequenceOptions.Mipmaps;

	if (InImageSequenceOptions.Format == "DXT1")
	{
		newImageSequence.Format = ImageFormat::DXT1;
	}
	else if (InImageSequenceOptions.Format == "DXT3")
	{
		newImageSequence.Format = ImageFormat::DXT3;
	}
	else if (InImageSequenceOptions.Format == "DXT5")
	{
		newImageSequence.Format = ImageFormat::DXT5;
	}

	// validate size
	newImageSequence.MaxSize = InImageSequenceOptions.MaxSize;
	if (newImageSequence.MaxSize != 128 &&
		newImageSequence.MaxSize != 256 &&
		newImageSequence.MaxSize != 512 &&
		newImageSequence.MaxSize != 1024 &&
		newImageSequence.MaxSize != 2048 &&
		newImageSequence.MaxSize != 4096 &&
		newImageSequence.MaxSize != 8192 &&
		newImageSequence.MaxSize != 16384)
	{
		std::printf("ERROR: Invalid maxSize specified for image sequence '%s'", filenameOnly.c_str());
		return;
	}


	std::list<std::string> files;
	for (const auto& entry : std::filesystem::directory_iterator(basePath))
	{
		files.push_back(entry.path().string());
	}

	files.sort();

	for (std::string& s : files)
	{
		newImageSequence.Files.push_back(s);
	}

	this->ImageSequences.push_back(newImageSequence);

}


//-----------------------------------------------------------------------------
// Converter::ProcessFrame
//-----------------------------------------------------------------------------
void Converter::ProcessFrame(int InFrameSaveIndex, int InFrameProcessIndex)
{

	std::shared_ptr<Converter::Frame> newFrame = std::make_shared<Converter::Frame>();

	newFrame->FrameIndex = InFrameSaveIndex;
	newFrame->Meshes.resize(this->Meshes.size());
	newFrame->Images.resize(this->ImageSequences.size());

	// build meshes for this frame
	for (int iMesh = 0; iMesh < this->Meshes.size(); iMesh++)
	{
		AbcArchiveMesh& m = this->Meshes[iMesh];
		FrameMeshData& meshData = newFrame->Meshes[iMesh];

		this->GenerateFrameMeshData(m, InFrameProcessIndex, meshData);

		if (this->Options.MeshOptimization)
		{
			this->OptimizeFrameMeshData(meshData);
		}

		// after the mesh has been optimized, check if we need to generate tangents (requires normals + first set of texture coords)
		if (this->Options.TangentFormat_ != TangentFormat::None )
		{
			bool bOptionsValid = this->Options.NormalFormat_ != NormalFormat::None && this->Options.TexCoordFormat_ != TexCoordFormat::None;
			bool bDataValid = meshData.Normals.size() > 0 && meshData.UVChannels[0].size() > 0;

			if (bOptionsValid && bDataValid)
			{
				this->GenerateTangentsOnFrameMesh(meshData);
			}
			else
			{
				if (!this->TangentWarningRaised)
				{
					this->RaiseWarning(Warnings::InsufficentDataToGenerateTangents);
				}
			}
		}

		// scale the mesh if necessary
		if (this->Options.Scale != 1.0f && 
			this->Options.Scale > 0.0f)
		{
			for (Vector3& v : meshData.Positions)
			{
				v *= this->Options.Scale;
			}

			for (Vector3& v : meshData.Velocities)
			{
				v *= this->Options.Scale;
			}

			// scale bounds
			meshData.BoundingCenter *= this->Options.Scale;
			meshData.BoundingSize *= this->Options.Scale;
		}

		// adjust velocity to match the frame rate
		{
			for (Vector3& v : meshData.Velocities) 
			{ 
				v *= this->TimePerFrame; 
			}
		}

		if (this->Options.Swizzle_ != Swizzle::None)
		{
			if (this->Options.Swizzle_ == Swizzle::XZ)
			{
				// vertex elements
				for (Vector3& v : meshData.Positions) { v.SwizzleXZ(); }
				for (Vector3& v : meshData.Normals) { v.SwizzleXZ(); }
				//for (Vector4& v : meshData.Tangents) { v.SwizzleXZ(); }	// swizzling handled in ::GenerateTangentsOnFrameMesh
				for (Vector3& v : meshData.Velocities) { v.SwizzleXZ(); }

				// bounds
				meshData.BoundingCenter.SwizzleXZ();
				meshData.BoundingSize.SwizzleXZ();

			}
			else if (this->Options.Swizzle_ == Swizzle::YZ)
			{
				// vertex elements
				for (Vector3& v : meshData.Positions) { v.SwizzleYZ(); }
				for (Vector3& v : meshData.Normals) { v.SwizzleYZ(); }
				//for (Vector4& v : meshData.Tangents) { v.SwizzleYZ(); }	// swizzling handled in ::GenerateTangentsOnFrameMesh
				for (Vector3& v : meshData.Velocities) { v.SwizzleYZ(); }

				// bounds
				meshData.BoundingCenter.SwizzleYZ();
				meshData.BoundingSize.SwizzleYZ();
			}
		}


		if (this->Options.FlipIndiceOrder)
		{
			uint32* pIndice = meshData.Indices.data();
			for (uint32 iSurface = 0; iSurface < meshData.Surfaces; iSurface++)
			{
				uint32 tmp = pIndice[1];
				pIndice[1] = pIndice[2];
				pIndice[2] = tmp;

				pIndice += 3;
			}
		}

		if (this->Options.FlipTextureCoords)
		{
			for (int32 iTC = 0; iTC < meshData.UVCount; iTC++)
			{
				for (Kimura::Vector2& uv : meshData.UVChannels[iTC])
				{
					uv.Y = 1.0f - uv.Y;
				}
			}
		}

		// pack all of the vertex elements into their respective formats based on the conversion options
		this->PackFrameMeshData(meshData);

		newFrame->TotalVertices += (uint32) meshData.Positions.size();
		newFrame->TotalSurfaces += meshData.Surfaces;

	}

	// build textures for this frame
	for (int iImageSequence = 0; iImageSequence < this->ImageSequences.size(); iImageSequence++)
	{
		InputImageSequence& iis = this->ImageSequences[iImageSequence];
		FrameImageData& fid = newFrame->Images[iImageSequence];

		this->GenerateFrameImageData(iis, InFrameProcessIndex, fid);
	}

	// store the frame
	{
		std::unique_lock<std::mutex> threadLock(this->FrameProcessingMutex);
		this->Frames[InFrameSaveIndex] = newFrame;
	}

	//std::printf("build frame %d, Vertices=%d, Surfaces =%d\n", newFrame->FrameIndex, newFrame->TotalVertices, newFrame->TotalSurfaces);

}


//-----------------------------------------------------------------------------
// Converter::GenerateFrameMeshData
//-----------------------------------------------------------------------------
void Converter::GenerateFrameMeshData(AbcArchiveMesh& InMesh, int InFrameIndex, FrameMeshData& OutRawMesh)
{
	Alembic::Abc::ISampleSelector sampleSelector((InFrameIndex + 1) * this->TimePerFrame);

	const Alembic::Abc::MetaData metadata = InMesh.AbcObject.getMetaData();

	// we support either subD or poly meshes
	if (Alembic::AbcGeom::ISubD::matches(metadata))
	{
		Alembic::AbcGeom::ISubD subDGeom = Alembic::AbcGeom::ISubD(InMesh.AbcObject, Alembic::Abc::kWrapExisting);
		const Alembic::AbcGeom::ISubDSchema subDSchema(subDGeom.getSchema());

		this->PopulateRawMeshDataFromPolyMeshSchema(InMesh.AbcObject, nullptr, &subDSchema, sampleSelector, OutRawMesh, InFrameIndex == this->StartFrame ? true : false);

		subDGeom.reset();

	}
	else if (Alembic::AbcGeom::IPolyMesh::matches(metadata))
	{
		Alembic::AbcGeom::IPolyMesh polyMeshGeom = Alembic::AbcGeom::IPolyMesh(InMesh.AbcObject, Alembic::Abc::kWrapExisting);
		const Alembic::AbcGeom::IPolyMeshSchema polyMeshSchema(polyMeshGeom.getSchema());

		this->PopulateRawMeshDataFromPolyMeshSchema(InMesh.AbcObject, &polyMeshSchema, nullptr, sampleSelector, OutRawMesh, InFrameIndex == this->StartFrame ? true : false);

		polyMeshGeom.reset();
	}

}


//-----------------------------------------------------------------------------
// Converter::CopyAbcElementsToKimuraElements
//-----------------------------------------------------------------------------
template<typename T, typename U> bool Converter::CopyAbcElementsToKimuraElements
	(
	
		T InAbcElements, 
		std::vector<U>& OutKimuraElements
		
	)
{
	const size_t numElements = InAbcElements->size();
	
	if (numElements == 0)
	{
		return false;
	}

	OutKimuraElements.resize(numElements);

	auto pSource = InAbcElements->get();
	auto pDestination = &OutKimuraElements[0];

	// Ensure that the destination and source data size corresponds (otherwise we will end up with an invalid memcpy and means we have a type mismatch)
	if (sizeof(pSource[0]) == sizeof(pDestination[0]))
	{
		memcpy(pDestination, pSource, sizeof(U) * numElements);
		return true;
	}

	return false;

}



//-----------------------------------------------------------------------------
// Converter::TriangulateBuffer
//-----------------------------------------------------------------------------
template <typename T> void Converter::TriangulateBuffer(const std::vector<unsigned int>& InNumIndicesPerSurface, std::vector<T>& InOutBuffer)
{

	// at this point, we assume that the surfaces are made out of either triangles or quads

	std::vector<T> newBuffer;
	newBuffer.reserve(InNumIndicesPerSurface.size() * 4);

	unsigned int index = 0;
	for (const unsigned int indicesForSurface : InNumIndicesPerSurface)
	{
		if (indicesForSurface == 3)
		{
			// just copy existing triangle
			newBuffer.push_back(InOutBuffer[index]);
			newBuffer.push_back(InOutBuffer[index + 1]);
			newBuffer.push_back(InOutBuffer[index + 2]);
		}
		else
		{
			// break the quad into two triangles
			newBuffer.push_back(InOutBuffer[index]);
			newBuffer.push_back(InOutBuffer[index + 1]);
			newBuffer.push_back(InOutBuffer[index + 3]);
			newBuffer.push_back(InOutBuffer[index + 3]);
			newBuffer.push_back(InOutBuffer[index + 1]);
			newBuffer.push_back(InOutBuffer[index + 2]);

		}

		index += indicesForSurface;
	}

	// update the source index buffer with a triangulated one
	InOutBuffer = newBuffer;

}


//-----------------------------------------------------------------------------
// Converter::ConvertToNonIndexedElements
//-----------------------------------------------------------------------------
template<typename T> void Converter::ConvertToNonIndexedElements(const std::vector<unsigned int>& InIndexBuffer, std::vector<T>& InOutElements)
{

	std::vector<T> nonIndexedElements;
	nonIndexedElements.reserve(InIndexBuffer.size());

	unsigned int incomingElementsSize = (unsigned int)InOutElements.size();

	for (unsigned int i : InIndexBuffer)
	{
		if (i >= incomingElementsSize)
		{
			i = incomingElementsSize - 1;
		}

		nonIndexedElements.push_back(InOutElements[i]);
	}

	InOutElements = nonIndexedElements;
}



//-----------------------------------------------------------------------------
// Converter::PopulateRawMeshDataFromPolyMeshSchema
//-----------------------------------------------------------------------------
bool Converter::PopulateRawMeshDataFromPolyMeshSchema
	(

		Alembic::Abc::IObject InObject, 
		const Alembic::AbcGeom::IPolyMeshSchema* InPolyMeshSchema,
		const Alembic::AbcGeom::ISubDSchema* InSubDSchema,
		const Alembic::Abc::ISampleSelector InFrameSelector,
		FrameMeshData& OutRawMeshFrameData,
		const bool InForceFirstFrame

	)
{



	Alembic::AbcGeom::IPolyMeshSchema::Sample polyMeshSample;
	Alembic::AbcGeom::IPolyMeshSchema::Sample* pPolyMeshSample = nullptr;
	if (InPolyMeshSchema)
	{
		InPolyMeshSchema->get(polyMeshSample, InFrameSelector);
		pPolyMeshSample = &polyMeshSample;
	}

	Alembic::AbcGeom::ISubDSchema::Sample subDSample;
	Alembic::AbcGeom::ISubDSchema::Sample* pSubDSample = nullptr;
	if (InSubDSchema)
	{
		InSubDSchema->get(subDSample, InFrameSelector);
		pSubDSample = &subDSample;

	}

	// update bounding box
	Alembic::Abc::Box3d boundingBox;
	{
		Alembic::Abc::IBox3dProperty p = InPolyMeshSchema != nullptr ? InPolyMeshSchema->getSelfBoundsProperty() : InSubDSchema->getSelfBoundsProperty();
		p.get(boundingBox, InFrameSelector);

		const Imath::V3d boundingBoxSize = boundingBox.size();
		const Imath::V3d boundingBoxCenter = boundingBox.center();

		OutRawMeshFrameData.BoundingCenter.X = (float)boundingBoxCenter.x;
		OutRawMeshFrameData.BoundingCenter.Y = (float)boundingBoxCenter.y;
		OutRawMeshFrameData.BoundingCenter.Z = (float)boundingBoxCenter.z;

		OutRawMeshFrameData.BoundingSize.X = (float)boundingBoxSize.x * 0.5f;
		OutRawMeshFrameData.BoundingSize.Y = (float)boundingBoxSize.y * 0.5f;
		OutRawMeshFrameData.BoundingSize.Z = (float)boundingBoxSize.z * 0.5f;

	}


	bool bMeshHasQuads = false;

	// 
	std::vector<unsigned int> indiceCountPerSurface;
	{
		Alembic::Abc::Int32ArraySamplePtr faceCounts = pPolyMeshSample != nullptr ? pPolyMeshSample->getFaceCounts() : pSubDSample->getFaceCounts();
		bool bSuccess = this->CopyAbcElementsToKimuraElements<Alembic::Abc::Int32ArraySamplePtr, unsigned int>(faceCounts, indiceCountPerSurface);

		if (!bSuccess)
		{
			return false;
		}

		// validate the polygons; support triangles and quads
		for (const unsigned int& i : indiceCountPerSurface)
		{
			if (i == 4)
			{
				// we support quads but extracting data will require conversion to triangles
				bMeshHasQuads = true;
			}
			else if (i != 3)
			{
				// must be either 4 or 3 vertices per surface. 
				// report invalid mesh due to invalid polygon surfaces
				this->RaiseWarning(Warnings::InvalidPolygonsDetected);

				return false;
			}
		}
	}


	// get the mesh's index buffer and re-triangulate it if necessary (contains quads)
	{
		Alembic::Abc::Int32ArraySamplePtr faceIndices = pPolyMeshSample != nullptr ? pPolyMeshSample->getFaceIndices() : pSubDSample->getFaceIndices();
		bool bSuccess = this->CopyAbcElementsToKimuraElements<Alembic::Abc::Int32ArraySamplePtr, unsigned int>(faceIndices, OutRawMeshFrameData.Indices);

		// 
		if (bMeshHasQuads)
		{
			// warn user about triangulating the mesh
			this->RaiseWarning(Warnings::PolygonConversionRequired);

			this->TriangulateBuffer<unsigned int>(indiceCountPerSurface, OutRawMeshFrameData.Indices);
		}
	}

	// we now know the number of surfaces this mesh has since it is made out of only triangles
	OutRawMeshFrameData.Surfaces = (int)OutRawMeshFrameData.Indices.size() / 3;

	// extract positions
	int numPositionsOriginally = 0;
	{
		Alembic::Abc::P3fArraySamplePtr meshPositions = pPolyMeshSample != nullptr ? pPolyMeshSample->getPositions() : pSubDSample->getPositions();
		bool bSuccess = this->CopyAbcElementsToKimuraElements<Alembic::Abc::P3fArraySamplePtr, Kimura::Vector3>(meshPositions, OutRawMeshFrameData.Positions);

		if (!bSuccess)
		{
			return false;
		}

		// save number of position elements there were initially before 
		numPositionsOriginally = (int)OutRawMeshFrameData.Positions.size();

		this->ConvertToNonIndexedElements(OutRawMeshFrameData.Indices, OutRawMeshFrameData.Positions);
	}


	// extract normals
	if (InPolyMeshSchema != nullptr)
	{
		Alembic::AbcGeom::IN3fGeomParam normalsParam = InPolyMeshSchema->getNormalsParam();

		if (normalsParam.valid())
		{
			this->ExtractElementsFromGeomParam<IN3fGeomParam, N3fArraySamplePtr, Kimura::Vector3>(InFrameSelector, normalsParam, OutRawMeshFrameData.Normals, indiceCountPerSurface, OutRawMeshFrameData.Indices, numPositionsOriginally, bMeshHasQuads);
		}

	}

	// extract UV channels
	{
		Alembic::AbcGeom::IV2fGeomParam uvParam = InPolyMeshSchema != nullptr ? InPolyMeshSchema->getUVsParam() : InSubDSchema->getUVsParam();
		if (uvParam.valid())
		{
			this->ExtractElementsFromGeomParam<IV2fGeomParam, V2fArraySamplePtr, Kimura::Vector2>(InFrameSelector, uvParam, OutRawMeshFrameData.UVChannels[OutRawMeshFrameData.UVCount++], indiceCountPerSurface, OutRawMeshFrameData.Indices, numPositionsOriginally, bMeshHasQuads);
		}
		else
		{
			// store 
//			OutRawMeshFrameData.UVChannels[0].resize(OutRawMeshFrameData.Indices.size());
		}

		// extra UV channels are stored in extra geom params
		ICompoundProperty geomParams = InPolyMeshSchema != nullptr ? InPolyMeshSchema->getArbGeomParams() : InSubDSchema->getArbGeomParams();
		if (geomParams.valid())
		{
			for (int iGeom = 0; iGeom < geomParams.getNumProperties(); ++iGeom)
			{
				auto p = geomParams.getPropertyHeader(iGeom);
				if (IV2fGeomParam::matches(p))
				{
					IV2fGeomParam uvExtraParam = IV2fGeomParam(geomParams, p.getName());
					this->ExtractElementsFromGeomParam<IV2fGeomParam, V2fArraySamplePtr, Kimura::Vector2>(InFrameSelector, uvExtraParam, OutRawMeshFrameData.UVChannels[OutRawMeshFrameData.UVCount++], indiceCountPerSurface, OutRawMeshFrameData.Indices, numPositionsOriginally, bMeshHasQuads);
				}

				// if we've extracted the maximum number of UVs possible..
				if (OutRawMeshFrameData.UVCount == Kimura::MaxTextureCoords)
				{
					break;
				}

			}
		}
	
	}

	// extract colors
	{
		ICompoundProperty geomParams = InPolyMeshSchema != nullptr ? InPolyMeshSchema->getArbGeomParams() : InSubDSchema->getArbGeomParams();
		if (geomParams.valid())
		{
			for (int iGeom = 0; iGeom < geomParams.getNumProperties(); iGeom++)
			{
				auto p = geomParams.getPropertyHeader(iGeom);
				if (IC3fGeomParam::matches(p))
				{
					IC3fGeomParam colorParam = IC3fGeomParam(geomParams, p.getName());

					std::vector<Vector3>	tmpVector;

					// extract as Vector3 first
					this->ExtractElementsFromGeomParam<IC3fGeomParam, C3fArraySamplePtr, Kimura::Vector3>
					(
						InFrameSelector, 
						colorParam,
						tmpVector,
						indiceCountPerSurface, 
						OutRawMeshFrameData.Indices, 
						(int)polyMeshSample.getPositions()->size(), 
						bMeshHasQuads
					);

					// then copy to vector4, and swap red and blue
					OutRawMeshFrameData.Colors[OutRawMeshFrameData.ColorCount].resize(tmpVector.size());
					Vector4* pVectorOut = OutRawMeshFrameData.Colors[OutRawMeshFrameData.ColorCount].data();
					for (const Vector3& v : tmpVector)
					{
						*pVectorOut++ = Vector4(v.X, v.Y, v.Z, 1.0f);
					}

					OutRawMeshFrameData.ColorCount++;

				}
				else if (IC4fGeomParam::matches(p))
				{
					IC4fGeomParam colorParam = IC4fGeomParam(geomParams, p.getName());

					this->ExtractElementsFromGeomParam<IC4fGeomParam, C4fArraySamplePtr, Kimura::Vector4>
						(
							InFrameSelector,
							colorParam,
							OutRawMeshFrameData.Colors[OutRawMeshFrameData.ColorCount],
							indiceCountPerSurface,
							OutRawMeshFrameData.Indices,
							(int)polyMeshSample.getPositions()->size(),
							bMeshHasQuads
							);

					// swap red and blue
// 					for (Vector4& v : OutRawMeshFrameData.Colors[OutRawMeshFrameData.ColorCount])
// 					{
// 						float t = v.Z;
// 						v.Z = v.X;
// 						v.X = t;
// 					}

					OutRawMeshFrameData.ColorCount++;
				}

				// if we've extracted the maximum number of color channels possible..
				if (OutRawMeshFrameData.ColorCount == Kimura::MaxColorChannels)
				{
					break;
				}

			}

		}

	}

	// extract velocities
	{
		Alembic::Abc::V3fArraySamplePtr meshVelocities = pPolyMeshSample != nullptr ? pPolyMeshSample->getVelocities() : pSubDSample->getVelocities();
		if (meshVelocities != nullptr)
		{
			bool bSuccess = this->CopyAbcElementsToKimuraElements<Alembic::Abc::V3fArraySamplePtr, Kimura::Vector3>(meshVelocities, OutRawMeshFrameData.Velocities);

			if (!bSuccess)
			{
				return false;
			}

			if (OutRawMeshFrameData.Velocities.size() != OutRawMeshFrameData.Positions.size())
			{
				this->ConvertToNonIndexedElements(OutRawMeshFrameData.Indices, OutRawMeshFrameData.Velocities);
			}
		}
	}

	return true;
}


//-----------------------------------------------------------------------------
// Converter::ExtractElementsFromGeomParam
//-----------------------------------------------------------------------------
template <typename abcParamType, typename abcElementType, typename kimuraElementType>
void Converter::ExtractElementsFromGeomParam(const Alembic::Abc::ISampleSelector InFrameSelector, abcParamType p, std::vector<kimuraElementType>& outputData, std::vector<unsigned int>& indiceCountPerSurface, std::vector<unsigned int>& defaultIndices, int defaultNumElements, bool meshHasQuads)
{

	abcElementType abcElements = p.getValueProperty().getValue(InFrameSelector);
	this->CopyAbcElementsToKimuraElements<abcElementType, kimuraElementType>(abcElements, outputData);

	// are the elements indexed?
	if (p.getIndexProperty().valid())
	{
		Alembic::Abc::UInt32ArraySamplePtr abcIndices = p.getIndexProperty().getValue(InFrameSelector);

		std::vector<unsigned int> kimuraIndices;
		this->CopyAbcElementsToKimuraElements<Alembic::Abc::UInt32ArraySamplePtr, unsigned int>(abcIndices, kimuraIndices);

		if (meshHasQuads)
		{
			this->TriangulateBuffer<unsigned int>(indiceCountPerSurface, kimuraIndices);
		}

		// 
		this->ConvertToNonIndexedElements<kimuraElementType>(kimuraIndices, outputData);
	}
	else
	{
		if (outputData.size() == defaultNumElements)
		{
			// re-use the mesh's index buffer for positions
			this->ConvertToNonIndexedElements<kimuraElementType>(defaultIndices, outputData);
		}
		else
		{
			if (outputData.size() != defaultIndices.size() && outputData.size() == defaultNumElements)
			{
				this->ConvertToNonIndexedElements<kimuraElementType>(defaultIndices, outputData);
			}
			else
			{
				// or triangulate the normals directly
				this->TriangulateBuffer<kimuraElementType>(indiceCountPerSurface, outputData);
			}
		}
	}
}


enum class TriangleFlags
{
	Degenerate = 1, 
	ToBeVisited = 2, 
	Visited = 4, 
	Side0Set = 16,
	Side1Set = 32,
	Side2Set = 64
};


struct OptimizationTriangle
{
	uint32 ConnectedTriangles[3];
	uint32 IndicesUsed[3];

	uint8	Flags;

	inline void AddTriangleConnection(uint32 iTriangle)
	{
		if (this->Flags & (uint8)TriangleFlags::Side0Set)
		{
			this->ConnectedTriangles[0] = iTriangle;
			this->Flags |= (uint8)TriangleFlags::Side0Set;
		}
		else if (this->Flags & (uint8)TriangleFlags::Side1Set)
		{
			this->ConnectedTriangles[1] = iTriangle;
			this->Flags |= (uint8)TriangleFlags::Side1Set;
		}
		else if (this->Flags & (uint8)TriangleFlags::Side2Set)
		{
			this->ConnectedTriangles[2] = iTriangle;
			this->Flags |= (uint8)TriangleFlags::Side2Set;
		}
	}

	inline uint32 GetNumTriangleConnections()
	{
		uint32 n = 0;
		if (this->Flags & (uint8)TriangleFlags::Side0Set) n++;
		if (this->Flags & (uint8)TriangleFlags::Side1Set) n++;
		if (this->Flags & (uint8)TriangleFlags::Side2Set) n++;
		return n;
	}

	inline void SetDegenerate()
	{
		this->Flags |= (uint32)TriangleFlags::Degenerate;
	}
	inline bool IsDegenerate()
	{
		return (this->Flags & (uint32)TriangleFlags::Degenerate);
	}

	inline void SetToBeVisited()
	{
		this->Flags |= (uint32)TriangleFlags::ToBeVisited;
	}

	inline void UnsetToBeVisited()
	{
		this->Flags ^= (uint32)TriangleFlags::ToBeVisited;
	}

	inline bool IsToBeVisited()
	{
		return (this->Flags & (uint32)TriangleFlags::ToBeVisited);
	}

	inline void SetVisited()
	{
		this->Flags |= (uint32)TriangleFlags::Visited;
	}

	inline bool WasVisited()
	{
		return (this->Flags & (uint32)TriangleFlags::Visited);
	}

	inline bool CanBeVisited()
	{
		return ((this->Flags & 0x0f) == 0);
	}

};


//-----------------------------------------------------------------------------
// Converter::OptimizeFrameMeshData
//-----------------------------------------------------------------------------
void Converter::OptimizeFrameMeshData(FrameMeshData& InOutRawMesh)
{
	
	uint32 sizeoftriangle = sizeof(OptimizationTriangle);

	// The incoming FrameMeshData has all of its vertex components completely unrolled. 
	// Thereforce, maxVertices = numTriangles * 3.
	int maxVertices = (int)InOutRawMesh.Positions.size();

	// final vertex buffer size will be less or equal to maxVertices
	std::vector<OptimizationVertex> newVertexBuffer;
	newVertexBuffer.reserve(maxVertices);

	// final index buffer size will be equal to maxVertices (numTriangles * 3)
	std::vector<unsigned int> newIndexBuffer;
	newIndexBuffer.reserve(maxVertices);

	std::vector<OptimizationTriangle> newTriangleLinks;
	newTriangleLinks.resize(maxVertices / 3);
	
	std::unordered_map<uint64, uint32>	edgeToTriangleConnections;

	OptimizationGraphNode* graphNodes = new OptimizationGraphNode[maxVertices];
	memset(graphNodes, 0, sizeof(OptimizationGraphNode) * maxVertices);
	int numGraphNodesUsed = 0;

	bool bHasNormals = InOutRawMesh.Normals.size() > 0;
	bool bHasVelocities = InOutRawMesh.Velocities.size() > 0;
	bool bHasTextureCoords0 = InOutRawMesh.UVChannels[0].size() > 0;
	bool bHasTextureCoords1 = InOutRawMesh.UVChannels[1].size() > 0;
	bool bHasTextureCoords2 = InOutRawMesh.UVChannels[2].size() > 0;
	bool bHasTextureCoords3 = InOutRawMesh.UVChannels[3].size() > 0;
	bool bHasColors0 = InOutRawMesh.Colors[0].size() > 0;
	bool bHasColors1 = InOutRawMesh.Colors[1].size() > 0;

	uint32 iVertInTriangle = 0;
	uint32* pCurrentTriangleIndices = newIndexBuffer.data();

	uint32 numInvalidTris = 0;

	// by going through all the vertices, we're effectively going through all the triangles
	for (int iVertex = 0; iVertex < maxVertices; iVertex++)
	{
		// assemble new vertex
		OptimizationVertex newVertex;
		{
			newVertex.P = InOutRawMesh.Positions[iVertex];
			newVertex.N = InOutRawMesh.Normals.size() == maxVertices ? InOutRawMesh.Normals[iVertex] : Vector3::ZeroVector;
			newVertex.V = InOutRawMesh.Velocities.size() == maxVertices ? InOutRawMesh.Velocities[iVertex] : Vector3::ZeroVector;
			newVertex.TextureCoords[0] = InOutRawMesh.UVChannels[0].size() == maxVertices ? InOutRawMesh.UVChannels[0][iVertex] : Vector2::ZeroVector;
			newVertex.TextureCoords[1] = InOutRawMesh.UVChannels[1].size() == maxVertices ? InOutRawMesh.UVChannels[1][iVertex] : Vector2::ZeroVector;
			newVertex.TextureCoords[2] = InOutRawMesh.UVChannels[2].size() == maxVertices ? InOutRawMesh.UVChannels[2][iVertex] : Vector2::ZeroVector;
			newVertex.TextureCoords[3] = InOutRawMesh.UVChannels[3].size() == maxVertices ? InOutRawMesh.UVChannels[3][iVertex] : Vector2::ZeroVector;
			newVertex.Colors[0] = InOutRawMesh.Colors[0].size() == maxVertices ? InOutRawMesh.Colors[0][iVertex] : Vector4::ZeroVector;
			newVertex.Colors[1] = InOutRawMesh.Colors[1].size() == maxVertices ? InOutRawMesh.Colors[1][iVertex] : Vector4::ZeroVector;
		}

		if (iVertex == 0)
		{
			// first vertex gets added automatically
			OptimizationGraphNode& firstNode = graphNodes[0];
			firstNode.Vertex = newVertex;
			firstNode.Index = 0;

			newVertexBuffer.push_back(newVertex);
			newIndexBuffer.push_back(firstNode.Index);

			numGraphNodesUsed = 1;

		}
		else
		{
			OptimizationGraphNode* pGraphNode = &graphNodes[0];

			while (true)
			{
				if (pGraphNode->Vertex.Equals(newVertex))
				{
					// vertex is already in the graph, simply save its indice
					newIndexBuffer.push_back(pGraphNode->Index);
					break;
				}

				// determine where in the graph to check or visit
				int iLeaf = 0;
				iLeaf += newVertex.P.X > pGraphNode->Vertex.P.X ? 1 : 0;
				iLeaf += newVertex.P.Y > pGraphNode->Vertex.P.Y ? 2 : 0;
				iLeaf += newVertex.P.Z > pGraphNode->Vertex.P.Z ? 4 : 0;
				if (bHasNormals)
				{
					iLeaf += newVertex.N.X > pGraphNode->Vertex.N.X ? 8 : 0;
					iLeaf += newVertex.N.Y > pGraphNode->Vertex.N.Y ? 16 : 0;
				}
				else if (bHasTextureCoords0)
				{
					iLeaf += newVertex.TextureCoords[0].X > pGraphNode->Vertex.TextureCoords[0].X ? 8 : 0;
					iLeaf += newVertex.TextureCoords[0].Y > pGraphNode->Vertex.TextureCoords[0].Y ? 16 : 0;
				}
				else if (bHasColors0)
				{
					iLeaf += newVertex.Colors[0].X > pGraphNode->Vertex.Colors[0].X ? 8 : 0;
					iLeaf += newVertex.Colors[0].Y > pGraphNode->Vertex.Colors[0].Y ? 16 : 0;
				}

				// new vertex found
				if (pGraphNode->leafs[iLeaf] == nullptr)
				{
					// create a new leaf
					pGraphNode->leafs[iLeaf] = &graphNodes[numGraphNodesUsed];

					// jump to new leaf and set its vertex data
					pGraphNode = pGraphNode->leafs[iLeaf];
					pGraphNode->Vertex = newVertex;
					pGraphNode->Index = numGraphNodesUsed;

					newVertexBuffer.push_back(newVertex);
					newIndexBuffer.push_back(numGraphNodesUsed);

					numGraphNodesUsed++;

					break;

				}
				else
				{
					// go further down the graph
					pGraphNode = pGraphNode->leafs[iLeaf];
				}

			}

		}

		// If mesh splitting optimization is enabled. 
		// Every 3 vertices (triangle), generate triangle edges and find connections to other existing triangles.
		if (this->Options.Force16bitIndices && ++iVertInTriangle == 3)
		{
			const uint32 a = pCurrentTriangleIndices[0];
			const uint32 b = pCurrentTriangleIndices[1];
			const uint32 c = pCurrentTriangleIndices[2];

			uint32 thisTriangleIndex = iVertex / 3;

			OptimizationTriangle& newTriangle = newTriangleLinks[thisTriangleIndex];

			newTriangle.IndicesUsed[0] = a;
			newTriangle.IndicesUsed[1] = b;
			newTriangle.IndicesUsed[2] = c;

			if (a == b || a == c || b == c)
			{
				newTriangle.SetDegenerate();
				numInvalidTris++;
			}
			else
			{
				// find edge associations
				{

					newTriangle.ConnectedTriangles[0] = 0;
					newTriangle.ConnectedTriangles[1] = 0;
					newTriangle.ConnectedTriangles[2] = 0;

					// generate keys for the edges using vertex indices. Each edge uses two 32bit indices. Combine the two separate indices into a 64bit key, but always 
					// keep the lowest of the two indice values in the key's first 32bit. 
					uint64 edgeKeys[3];
					edgeKeys[0] = (a < b) ? (((uint64)a << 32) + b) : (((uint64)b << 32) + a);
					edgeKeys[1] = (b < c) ? (((uint64)b << 32) + c) : (((uint64)c << 32) + b);
					edgeKeys[2] = (c < a) ? (((uint64)c << 32) + a) : (((uint64)a << 32) + c);

					// find triangle associations by edges
					for (int iEdge = 0; iEdge < 3; iEdge++)
					{
						auto it = edgeToTriangleConnections.find(edgeKeys[iEdge]);
						if (it != edgeToTriangleConnections.end())
						{
							// edge already associated with another triangle
							uint32 iConnection = it->second;

							newTriangle.AddTriangleConnection(iConnection);

							// get the other triangle
							OptimizationTriangle& otherTriangle = newTriangleLinks[iConnection];

							otherTriangle.AddTriangleConnection(thisTriangleIndex);

							// two triangles shared this edge so we can remove the edge from the map. New triangles are free to 
							// re-add the edge once again.
							edgeToTriangleConnections.erase(it);

						}
						else
						{
							edgeToTriangleConnections.insert({ edgeKeys[iEdge], thisTriangleIndex });
						}
					}
				}

			}

			iVertInTriangle = 0;
			pCurrentTriangleIndices += 3;
		}

	}

	// we're done with the graph nodes
	delete[] graphNodes;

	// This map can consume quite a lot of memory but we're done with it. Free whatever memory we can as soon as possible.
	edgeToTriangleConnections.clear();


	// At this point, we have:
	//	- An array of unique vertices (newVertexBuffer)
	//  - A triangle list of indices pointing to the unique vertices
	//  - Possibly a database of triangle connections (depending on optimization options) 


	// split the newVertexBuffer and newIndexBuffer into smaller meshes limited to 64k vertices (16bit indices)
	if (this->Options.Force16bitIndices)
	{

		// re-order triangles so that they remain as connected as possible

		struct SplitGeometry
		{
			std::vector<OptimizationVertex> subVertexBuffer;
			std::vector<unsigned int> subIndexBuffer;
		};

		std::vector<SplitGeometry> geometryBuffers;
		SplitGeometry* pCurrentGB = nullptr;

		std::vector<int32>	remappedIndices(newIndexBuffer.size());

		std::vector<OptimizationTriangle>& InTriangles = newTriangleLinks;

		{

			uint32 iLinearVisit = 0;
			uint32 numTrianglesToVisit = (uint32)InTriangles.size();

			std::vector<uint32> buffer_a;
			std::vector<uint32> buffer_b;
			buffer_a.reserve(InTriangles.size());
			buffer_b.reserve(InTriangles.size());

			bool b = false;

			OptimizationTriangle* pLinearVisit = InTriangles.data();

			while (iLinearVisit < numTrianglesToVisit)
			{
				// on each loop iteration, we visit a list of triangles and generate a list of triangles to visit in the next iteration. 

				// we use two lists that are swapped on every iteration. 
				b = !b;
				std::vector<uint32>& visitBuffer = b ? buffer_a : buffer_b;
				std::vector<uint32>& nextBuffer = b ? buffer_b : buffer_a;

				// clear this buffer so that it may contain fresh new triangles to visit
				nextBuffer.clear();

				// find a triangle to visit if we don't have one yet
				if (visitBuffer.size() == 0)
				{
					while (iLinearVisit < numTrianglesToVisit)
					{
						if (pLinearVisit->CanBeVisited())
						{
							visitBuffer.push_back(iLinearVisit);
							pLinearVisit->SetToBeVisited();
							break;
						}

						pLinearVisit++;
						iLinearVisit++;
					}
				}


				for (uint32 iTriangleToVisit : visitBuffer)
				{
					OptimizationTriangle& t = InTriangles[iTriangleToVisit];

					t.SetVisited();

					// inject triangle vertices and indices here. 
					{

						if (!pCurrentGB)
						{
							// reset remapped indices
							for (int32& i : remappedIndices)
							{
								i = -1;
							}

							geometryBuffers.push_back(SplitGeometry());

							pCurrentGB = &geometryBuffers[geometryBuffers.size() - 1];
							pCurrentGB->subVertexBuffer.reserve(64 * 1024);
							pCurrentGB->subIndexBuffer.reserve(maxVertices);

						}

						// take the triangle, add its vertices and indices
						for (uint32 iIndice = 0; iIndice < 3; iIndice++)
						{
							if (remappedIndices[t.IndicesUsed[iIndice]] != -1)
							{
								// vertex already mapped in the current vertex buffer, reuse the indice to it
								pCurrentGB->subIndexBuffer.push_back((uint32)remappedIndices[t.IndicesUsed[iIndice]]);
							}
							else
							{
								// add new indice
								uint32 indice = (uint32)pCurrentGB->subVertexBuffer.size();
								pCurrentGB->subIndexBuffer.push_back(indice);
								remappedIndices[t.IndicesUsed[iIndice]] = (int32)indice;

								// add new vertex 
								pCurrentGB->subVertexBuffer.push_back(newVertexBuffer[t.IndicesUsed[iIndice]]);

							}

						}

					}

					// has the vertex buffer limit been reached?
					const uint32 c_VertexBufferSizeLimit = (64 * 1024) - 3;
					if (pCurrentGB->subVertexBuffer.size() >= c_VertexBufferSizeLimit)
					{
						// set current GB to null, this will force the addition of a new geometry buffer for a new section.
						pCurrentGB = nullptr;

						// Clear visit buffers. We want to start a new section from a single triangle.
						// Otherwise we might end up with long strips of triangles that duplicate lots of vertices. 
						{
							for (uint32 triangleId : visitBuffer)
							{
								OptimizationTriangle& triToRemove = InTriangles[triangleId];
								triToRemove.UnsetToBeVisited();
							}

							// clear next buffer
							for (uint32 triangleId : nextBuffer)
							{
								OptimizationTriangle& triToRemove = InTriangles[triangleId];
								triToRemove.UnsetToBeVisited();
							}

							visitBuffer.clear();
							nextBuffer.clear();
						}

						break;

					}

					// go through the triangle's edges and find connections to triangles that should be visited in the next loop 
					for (uint32 iConnection = 0; iConnection < 3; iConnection++)
					{
						
						uint32 i = t.ConnectedTriangles[iConnection];

						if (i == 0)
						{
							continue;
						}

						OptimizationTriangle& otherTriangle = InTriangles[i];
						if (otherTriangle.CanBeVisited())
						{
							nextBuffer.push_back(i);
							otherTriangle.SetToBeVisited();
						}

					}

				}

			}

		}

		// calculate total number of vertices, indices and surfaces. Create rendering sections from the different geometry buffers
		uint32 numVertices = 0;
		uint32 numIndices = 0;
		for (SplitGeometry& g : geometryBuffers)
		{
			FrameMeshData::Section s;
			s.IndexStart = numIndices;
			s.VertexStart = numVertices;

			numVertices += (uint32)g.subVertexBuffer.size();
			numIndices += (uint32)g.subIndexBuffer.size();

			s.MinVertexIndex = 0;
			s.MaxVertexIndex = (uint32)g.subVertexBuffer.size();
			s.NumSurfaces = (uint32)g.subIndexBuffer.size() / 3;

			InOutRawMesh.Sections.push_back(s);
		}


		newVertexBuffer.clear();
		newVertexBuffer.resize(numVertices);
		newIndexBuffer.clear();
		newIndexBuffer.reserve(numIndices);

		// DEV OPTION
		const bool bForceRandomColoredSections = false;

		if (bForceRandomColoredSections)
		{
			bHasColors0 = true;
			InOutRawMesh.ColorCount = 1;
			this->Options.ColorFormat_ = ColorFormat::Byte;
		}

		// 
		uint32 iGB = 0;
		OptimizationVertex* pV = newVertexBuffer.data();
		for (SplitGeometry& g : geometryBuffers)
		{

			if (bForceRandomColoredSections)
			{
				// generate a color to assign to all triangles from this mesh
				Vector4 col = { (float)(rand() % 100) / 100.0f, 
								(float)(rand() % 100) / 100.0f, 
								(float)(rand() % 100) / 100.0f, 
								1.0f };

				for (const OptimizationVertex& v : g.subVertexBuffer)
				{
					*pV = v;
					pV->Colors[0] = col;
					pV++;
				}

			}
			else
			{
				// TODO: replace by a simple memcpy

				for (const OptimizationVertex& v : g.subVertexBuffer)
				{
					*pV++ = v;
				}
			}


			for (const uint32& i : g.subIndexBuffer)
			{
				newIndexBuffer.push_back(i);
			}

			iGB++;
		}

		// using more than a single section signals that we're definitely limiting our indices to 16bit
		if (InOutRawMesh.Sections.size() > 1)
		{
			InOutRawMesh.Force16bitIndices = true;
		}

	}
	else
	{
		// create a single section containing all of the surfaces in the mesh
		FrameMeshData::Section s;
		s.IndexStart = 0;
		s.VertexStart = 0;
		s.MinVertexIndex = 0;
		s.MaxVertexIndex = (uint32)newVertexBuffer.size();
		s.NumSurfaces = (uint32)newIndexBuffer.size() / 3;

		InOutRawMesh.Sections.push_back(s);

		InOutRawMesh.Force16bitIndices = false;
	}

	// update in the raw mesh's vertex and index buffers with our re-organized ones
	{

		int newVertexBufferSize = (int)newVertexBuffer.size();

		// first, adjust size of vertex element arrays
		{
			InOutRawMesh.Positions.clear();
			InOutRawMesh.Positions.reserve(newVertexBufferSize);

			if (bHasNormals)
			{
				InOutRawMesh.Normals.clear();
				InOutRawMesh.Normals.reserve(newVertexBufferSize);
			}

			// looking for tangents? They are generated AFTER the optimization phase

			if (bHasVelocities)
			{
				InOutRawMesh.Velocities.clear();
				InOutRawMesh.Velocities.reserve(newVertexBufferSize);
			}

			if (bHasTextureCoords0)
			{
				InOutRawMesh.UVChannels[0].clear();
				InOutRawMesh.UVChannels[0].reserve(newVertexBufferSize);
			}

			if (bHasTextureCoords1)
			{
				InOutRawMesh.UVChannels[1].clear();
				InOutRawMesh.UVChannels[1].reserve(newVertexBufferSize);
			}

			if (bHasTextureCoords2)
			{
				InOutRawMesh.UVChannels[2].clear();
				InOutRawMesh.UVChannels[2].reserve(newVertexBufferSize);
			}

			if (bHasTextureCoords3)
			{
				InOutRawMesh.UVChannels[3].clear();
				InOutRawMesh.UVChannels[3].reserve(newVertexBufferSize);
			}

			if (bHasColors0)
			{
				InOutRawMesh.Colors[0].clear();
				InOutRawMesh.Colors[0].reserve(newVertexBufferSize);
			}

			if (bHasColors1)
			{
				InOutRawMesh.Colors[1].clear();
				InOutRawMesh.Colors[1].reserve(newVertexBufferSize);
			}
		}


		for (int iNewVertex = 0; iNewVertex < newVertexBufferSize; iNewVertex++)
		{

			OptimizationVertex& v = newVertexBuffer[iNewVertex];

			InOutRawMesh.Positions.push_back(v.P);

			if (bHasNormals)
				InOutRawMesh.Normals.push_back(v.N);

			if (bHasVelocities)
				InOutRawMesh.Velocities.push_back(v.V);

			if (bHasTextureCoords0)
				InOutRawMesh.UVChannels[0].push_back(v.TextureCoords[0]);

			if (bHasTextureCoords1)
				InOutRawMesh.UVChannels[1].push_back(v.TextureCoords[1]);

			if (bHasTextureCoords2)
				InOutRawMesh.UVChannels[2].push_back(v.TextureCoords[2]);

			if (bHasTextureCoords3)
				InOutRawMesh.UVChannels[3].push_back(v.TextureCoords[3]);

			if (bHasColors0)
				InOutRawMesh.Colors[0].push_back(v.Colors[0]);

			if (bHasColors1)
				InOutRawMesh.Colors[1].push_back(v.Colors[1]);

		}

		InOutRawMesh.Indices = newIndexBuffer;

		// update number of surfaces 
		InOutRawMesh.Surfaces = (uint32)newIndexBuffer.size() / 3;

	}

}


//-----------------------------------------------------------------------------
// Converter::PackFrameMeshData
//-----------------------------------------------------------------------------
void Converter::PackFrameMeshData(FrameMeshData& InOutMeshData)
{

	std::vector<byte>	FrameBuffer;
	uint64				PositionInFrameBuffer = 0;
	
	bool bUse32BitIndices = !this->Options.Force16bitIndices && (InOutMeshData.Positions.size() > 0xfffe);

	InOutMeshData.IndicesHash = StdVectorHash<uint32>(InOutMeshData.Indices);
	this->PackIndices(InOutMeshData.Indices, InOutMeshData.IndicesPacked, bUse32BitIndices);

	InOutMeshData.PositionsHash = ArrayHash<float>((float*)InOutMeshData.Positions.data(), (int)InOutMeshData.Positions.size() * 3);
	this->PackPositions(InOutMeshData.Positions, InOutMeshData.PositionsPacked, InOutMeshData);

	InOutMeshData.NormalsHash = ArrayHash<float>((float*)InOutMeshData.Normals.data(), (int)InOutMeshData.Normals.size() * 3);
	this->PackNormals(InOutMeshData.Normals, InOutMeshData.NormalsPacked);

	InOutMeshData.TangentsHash = ArrayHash<float>((float*)InOutMeshData.Tangents.data(), (int)InOutMeshData.Tangents.size() * 4);
	this->PackTangents(InOutMeshData.Tangents, InOutMeshData.TangentsPacked);

	InOutMeshData.VelocitiesHash = ArrayHash<float>((float*)InOutMeshData.Velocities.data(), (int)InOutMeshData.Velocities.size() * 3);
	this->PackVelocities(InOutMeshData.Velocities, InOutMeshData.VelocitiesPacked, InOutMeshData);

	for (uint32 iTexCoord = 0; iTexCoord < MaxTextureCoords; iTexCoord++)
	{
		InOutMeshData.UVChannelsHash[iTexCoord] = ArrayHash<float>((float*)InOutMeshData.UVChannels[iTexCoord].data(), (int)InOutMeshData.UVChannels[iTexCoord].size() * 2);
		this->PackTexCoords(InOutMeshData.UVChannels[iTexCoord], InOutMeshData.UVChannelsPacked[iTexCoord]);
	}

	for (uint32 iColor = 0; iColor < MaxColorChannels; iColor++)
	{
		InOutMeshData.ColorsHash[iColor] = ArrayHash<float>((float*)InOutMeshData.Colors[iColor].data(), (int)InOutMeshData.Colors[iColor].size() * 4);
		this->PackColors(InOutMeshData.Colors[iColor], InOutMeshData.ColorsPacked[iColor], InOutMeshData.ColorQuantizationExtents[iColor]);
	}
}


//-----------------------------------------------------------------------------
// Converter::PackIndices
//-----------------------------------------------------------------------------
void Converter::PackIndices(std::vector<uint32>& InIndices, std::vector<byte>& OutPackedData, bool InPack32bit)
{

	if (InIndices.empty())
	{
		return;
	}

	// format used depends on how many vertices are in each frame. The converter will only 
	// store 32bit indices only when the number of vertices is higher than 65k 
	if (InPack32bit)
	{
		// this frame required 32bit indices			
		OutPackedData.resize(InIndices.size() * 4);
		memcpy(OutPackedData.data(), InIndices.data(), InIndices.size() * 4);

	}
	else
	{

		// this frame can do with 16bit indices, convert from 32bit to 16bit. 

		OutPackedData.resize(InIndices.size() * 2);

		uint32* pInIndices = (uint32*)InIndices.data();
		uint16* pOutIndices = (uint16*)OutPackedData.data();
		for (uint32 i = 0; i < InIndices.size(); i++)
		{
			if (pInIndices[i] > 0xffff)
			{
				// TODO: raise error? Not sure this would even be allowed to happen.
				int error = 1;
				std::printf("Error: trying to pack a 32bit indice into a 16bit indice\n");

			}

			pOutIndices[i] = (uint16)pInIndices[i];
		}

	}

}


//-----------------------------------------------------------------------------
// QuantizeVectorsToInt16
//-----------------------------------------------------------------------------
void QuantizeVectorsToInt16
	(
	
		std::vector<Vector3>&	InPositions, 
		std::vector<byte>&		OutPackedData, 
		Vector3&				OutCenter, 
		Vector3&				OutExtent
		
	)
{
	// Find the extents of the positions in x, y, z
	Vector3 vMin(9999999.0f, 9999999.0f, 9999999.0f);
	Vector3 vMax(-9999999.0f, -9999999.0f, -9999999.0f);
	{

		for (const Vector3& v : InPositions)
		{
			vMin.X = vMin.X < v.X ? vMin.X : v.X;
			vMin.Y = vMin.Y < v.Y ? vMin.Y : v.Y;
			vMin.Z = vMin.Z < v.Z ? vMin.Z : v.Z;

			vMax.X = vMax.X > v.X ? vMax.X : v.X;
			vMax.Y = vMax.Y > v.Y ? vMax.Y : v.Y;
			vMax.Z = vMax.Z > v.Z ? vMax.Z : v.Z;

		}
	}

	// bounding box center and extents
	Vector3 vCenter((vMin.X + vMax.X) * 0.5f,
					(vMin.Y + vMax.Y) * 0.5f,
					(vMin.Z + vMax.Z) * 0.5f);

	Vector3 vExtents(	(vMax.X - vMin.X) * 0.5f,
						(vMax.Y - vMin.Y) * 0.5f,
						(vMax.Z - vMin.Z) * 0.5f);

	// allocate packed data
	OutPackedData.resize(InPositions.size() * 3 * sizeof(int16));

	// cast into int16* 
	int16* pQuantizedVectors = (int16*)OutPackedData.data();
	for (const Vector3& v : InPositions)
	{
		float a = (v.X - vCenter.X) / vExtents.X;
		if (a != a) 
		{
			if (vExtents.X != 0.0f)
			{
				int b = 1;
			}
			a = 0.0f;
		}
		pQuantizedVectors[0] = (int16)(a * 32767.0f);

		a = (v.Y - vCenter.Y) / vExtents.Y;
		if (a!=a) 
		{
			if (vExtents.Y != 0.0f)
			{
				int b = 1;
			}
			a = 0.0f;
		}
		pQuantizedVectors[1] = (int16)(a * 32767.0f);

		a = (v.Z - vCenter.Z) / vExtents.Z;
		if (a != a)
		{
			if (vExtents.Z != 0.0f)
			{
				int b = 1;
			}
			a = 0.0f;
		}
		pQuantizedVectors[2] = (int16)(a * 32767.0f);

		pQuantizedVectors += 3;
	}

	OutCenter = vCenter;
	OutExtent = vExtents;

}



//-----------------------------------------------------------------------------
// QuantizeColorsToUInt16
//-----------------------------------------------------------------------------
void QuantizeColorsToUInt16
	(
	
		std::vector<Vector4>&	InPositions, 
		std::vector<byte>&		OutPackedData, 
		Vector4&				OutExtent
		
	)
{
	// Find the extents of the positions in x, y, z
	Vector4 vMax(-9999999.0f, -9999999.0f, -9999999.0f, -9999999.0f);
	{

		for (const Vector4& v : InPositions)
		{
			vMax.X = vMax.X > v.X ? vMax.X : v.X;
			vMax.Y = vMax.Y > v.Y ? vMax.Y : v.Y;
			vMax.Z = vMax.Z > v.Z ? vMax.Z : v.Z;
			vMax.W = vMax.W > v.W ? vMax.W : v.W;

		}
	}

	Vector4 vExtents = vMax;

	// allocate packed data
	OutPackedData.resize(InPositions.size() * 4 * sizeof(int16));

	// cast into int16* 
	uint16* pQuantizedVectors = (uint16*)OutPackedData.data();
	for (const Vector4& v : InPositions)
	{
		// X
		float quantX = v.X / vExtents.X;
		if (quantX != quantX)
		{
			quantX = 0.0f;
		}
		pQuantizedVectors[0] = (uint16)(quantX * 65535.0f);

		// Y
		float quantY = v.Y / vExtents.Y;
		if (quantY != quantY)
		{
			quantY = 0.0f;
		}
		pQuantizedVectors[1] = (uint16)(quantY * 65535.0f);

		// Z
		float quantZ = v.Z / vExtents.Z;
		if (quantZ != quantZ)
		{
			quantZ = 0.0f;
		}
		pQuantizedVectors[2] = (uint16)(quantZ * 65535.0f);

		// W
		float quantW = v.W / vExtents.W;
		if (quantW != quantW)
		{
			quantW = 0.0f;
		}
		pQuantizedVectors[3] = (uint16)(quantW * 65535.0f);

		pQuantizedVectors += 4;
	}

	OutExtent = vExtents;

}



//-----------------------------------------------------------------------------
// QuantizeColorsToUInt8
//-----------------------------------------------------------------------------
void QuantizeColorsToUInt8
	(
	
		std::vector<Vector4>&	InPositions, 
		std::vector<byte>&		OutPackedData, 
		Vector4&				OutExtent
		
	)
{
	// Find the extents of the positions in x, y, z
	Vector4 vMax(-9999999.0f, -9999999.0f, -9999999.0f, -9999999.0f);
	{

		for (const Vector4& v : InPositions)
		{
			vMax.X = vMax.X > v.X ? vMax.X : v.X;
			vMax.Y = vMax.Y > v.Y ? vMax.Y : v.Y;
			vMax.Z = vMax.Z > v.Z ? vMax.Z : v.Z;
			vMax.W = vMax.W > v.W ? vMax.W : v.W;

		}
	}

	Vector4 vExtents = vMax;

	// allocate packed data
	OutPackedData.resize(InPositions.size() * 4 * sizeof(int16));

	// cast into int16* 
	uint8* pQuantizedVectors = (uint8*)OutPackedData.data();
	for (const Vector4& v : InPositions)
	{
		// X
		float quantX = v.X / vExtents.X;
		if (quantX != quantX)
		{
			quantX = 0.0f;
		}
		pQuantizedVectors[0] = (uint8)(quantX * 255.0f);

		// Y
		float quantY = v.Y / vExtents.Y;
		if (quantY != quantY)
		{
			quantY = 0.0f;
		}
		pQuantizedVectors[1] = (uint8)(quantY * 255.0f);

		// Z
		float quantZ = v.Z / vExtents.Z;
		if (quantZ != quantZ)
		{
			quantZ = 0.0f;
		}
		pQuantizedVectors[2] = (uint8)(quantZ * 255.0f);

		// W
		float quantW = v.W / vExtents.W;
		if (quantW != quantW)
		{
			quantW = 0.0f;
		}
		pQuantizedVectors[3] = (uint8)(quantW * 255.0f);

		pQuantizedVectors += 4;
	}

	OutExtent = vExtents;

}


//-----------------------------------------------------------------------------
// QuantizeVectorsToInt8
//-----------------------------------------------------------------------------
void QuantizeVectorsToInt8
	(
	
		std::vector<Vector3>&	InPositions, 
		std::vector<byte>&		OutPackedData, 
		Vector3&				OutCenter, 
		Vector3&				OutExtent
		
	)
{
	// Find the extents of the positions in x, y, z
	Vector3 vMin(9999999.0f, 9999999.0f, 9999999.0f);
	Vector3 vMax(-9999999.0f, -9999999.0f, -9999999.0f);
	{

		for (const Vector3& v : InPositions)
		{
			vMin.X = vMin.X < v.X ? vMin.X : v.X;
			vMin.Y = vMin.Y < v.Y ? vMin.Y : v.Y;
			vMin.Z = vMin.Z < v.Z ? vMin.Z : v.Z;

			vMax.X = vMax.X > v.X ? vMax.X : v.X;
			vMax.Y = vMax.Y > v.Y ? vMax.Y : v.Y;
			vMax.Z = vMax.Z > v.Z ? vMax.Z : v.Z;

		}
	}

	// bounding box center and extents
	Vector3 vCenter((vMin.X + vMax.X) * 0.5f,
					(vMin.Y + vMax.Y) * 0.5f,
					(vMin.Z + vMax.Z) * 0.5f);

	Vector3 vExtents(	(vMax.X - vMin.X) * 0.5f,
						(vMax.Y - vMin.Y) * 0.5f,
						(vMax.Z - vMin.Z) * 0.5f);

	// allocate packed data
	OutPackedData.resize(InPositions.size()*3*1);

	// cast into int8* 
	int8* pQuantizedVectors = (int8*)OutPackedData.data();
	for (const Vector3& v : InPositions)
	{
		float a = (v.X - vCenter.X) / vExtents.X;
		if (a != a) a = 0.0f;
		pQuantizedVectors[0] = (int8)(a * 127.0f);

		a = (v.Y - vCenter.Y) / vExtents.Y;
		if (a != a) a = 0.0f;
		pQuantizedVectors[1] = (int8)(a * 127.0f);

		a = (v.Z - vCenter.Z) / vExtents.Z;
		if (a != a) a = 0.0f;
		pQuantizedVectors[2] = (int8)(a * 127.0f);

		pQuantizedVectors += 3;
	}

	OutCenter = vCenter;
	OutExtent = vExtents;

}


//-----------------------------------------------------------------------------
// Converter::PackPositions
//-----------------------------------------------------------------------------
void Converter::PackPositions(std::vector<Vector3>& InPositions, std::vector<byte>& OutPackedData, FrameMeshData& InOutFrameMeshData)
{
	if (InPositions.empty())
	{
		return;
	}

	switch (this->Options.PositionFormat_)
	{
		case PositionFormat::Full:
		{
			uint32 uSize = (uint32)(InPositions.size() * 3 * sizeof(float));

			OutPackedData.resize(uSize);
			memcpy(OutPackedData.data(), InPositions.data(), uSize);

			InOutFrameMeshData.PositionQuantizationExtents = Vector3(1.0f, 1.0f, 1.0f);
			InOutFrameMeshData.PositionQuantizationCenter = Vector3(0.0f, 0.0f, 0.0f); 

			break;
		}

		case PositionFormat::Half:
		{
			QuantizeVectorsToInt16(	InPositions, 
									OutPackedData, 
									InOutFrameMeshData.PositionQuantizationCenter, 
									InOutFrameMeshData.PositionQuantizationExtents);

			break;
		}

		default:
		{
			break;
		}
	}
}


//-----------------------------------------------------------------------------
// Converter::PackNormals
//-----------------------------------------------------------------------------
void Converter::PackNormals(std::vector<Vector3>& InNormals, std::vector<byte>& OutPackedData)
{
	if (InNormals.empty() || this->Options.NormalFormat_ == NormalFormat::None)
	{
		return;
	}

	switch (this->Options.NormalFormat_)
	{
		case NormalFormat::Full:
		{
			uint32 uSize = (uint32)(InNormals.size() * 3 * sizeof(float));

			OutPackedData.resize(uSize);
			memcpy(OutPackedData.data(), InNormals.data(), uSize);

			break;
		}

		case NormalFormat::Half:
		{
			uint32 uSize = (uint32)(InNormals.size() * 3 * sizeof(int16));
			OutPackedData.resize(uSize);

			float* pInPositions = (float*)InNormals.data();
			int16* pOutPositions = (int16*)OutPackedData.data();
			// convert all floats (from range -1.0f to 1.0f) into int16's ranging from -32767 to 32767
			for (uint32 i = 0; i < InNormals.size() * 3; i++)
			{
				*pOutPositions++ = UnitFloatToInt16(*pInPositions++);
			}

			break;
		}

		case NormalFormat::Byte:
		{
			uint32 uSize = (uint32)(InNormals.size() * 3 * sizeof(int8));
			OutPackedData.resize(uSize);

			float* pInPositions = (float*)InNormals.data();
			int8* pOutPositions = (int8*)OutPackedData.data();
			// convert all floats (from range -1.0f to 1.0f) into int16's ranging from -127 to 127
			for (uint32 i = 0; i < InNormals.size() * 3; i++)
			{
				*pOutPositions++ = UnitFloatToInt8(*pInPositions++);
			}

			break;
		}

		case NormalFormat::None:
		default:
		{
			break;
		}

	}

}


void Converter::PackTangents(std::vector<Vector4>& InTangents, std::vector<byte>& OutPackedData)
{
	if (InTangents.empty() || this->Options.TangentFormat_ == TangentFormat::None)
	{
		return;
	}

	switch (this->Options.TangentFormat_)
	{
		case TangentFormat::Full:
		{
			uint32 uSize = (uint32)(InTangents.size() * 4 * sizeof(float));

			OutPackedData.resize(uSize);
			memcpy(OutPackedData.data(), InTangents.data(), uSize);

			break;
		}

		case TangentFormat::Half:
		{
			uint32 uSize = (uint32)(InTangents.size() * 4 * sizeof(int16));
			OutPackedData.resize(uSize);

			float* pInPositions = (float*)InTangents.data();
			int16* pOutPositions = (int16*)OutPackedData.data();
			// convert all floats (from range -1.0f to 1.0f) into int16's ranging from -32767 to 32767
			for (uint32 i = 0; i < InTangents.size() * 4; i++)
			{
				*pOutPositions++ = UnitFloatToInt16(*pInPositions++);
			}

			break;
		}

		case TangentFormat::Byte:
		{
			uint32 uSize = (uint32)(InTangents.size() * 4 * sizeof(int8));
			OutPackedData.resize(uSize);

			float* pInPositions = (float*)InTangents.data();
			int8* pOutPositions = (int8*)OutPackedData.data();
			// convert all floats (from range -1.0f to 1.0f) into int16's ranging from -127 to 127
			for (uint32 i = 0; i < InTangents.size() * 4; i++)
			{
				*pOutPositions++ = UnitFloatToInt8(*pInPositions++);
			}

			break;
		}

		case TangentFormat::None:
		default:
		{
			break;
		}

	}
}


//-----------------------------------------------------------------------------
// Converter::PackVelocities
//-----------------------------------------------------------------------------
void Converter::PackVelocities(std::vector<Vector3>& InVelocities, std::vector<byte>& OutPackedData, FrameMeshData& InOutFrameMeshData)
{
	if (InVelocities.empty() || this->Options.VelocityFormat_ == VelocityFormat::None)
	{
		return;
	}
	
	switch (this->Options.VelocityFormat_)
	{
		case VelocityFormat::Full:
		{
			uint32 uSize = (uint32)(InVelocities.size() * 3 * sizeof(float));

			OutPackedData.resize(uSize);
			memcpy(OutPackedData.data(), InVelocities.data(), uSize);

			InOutFrameMeshData.VelocityQuantizationExtents = Vector3(1.0f, 1.0f, 1.0f);
			InOutFrameMeshData.VelocityQuantizationCenter = Vector3(0.0f, 0.0f, 0.0f);

			break;
		}

		case VelocityFormat::Half:
		{
			QuantizeVectorsToInt16( InVelocities,
									OutPackedData, 
									InOutFrameMeshData.VelocityQuantizationCenter,
									InOutFrameMeshData.VelocityQuantizationExtents);

			break;
		}

		case VelocityFormat::Byte:
		{

			QuantizeVectorsToInt8(	InVelocities,
									OutPackedData, 
									InOutFrameMeshData.VelocityQuantizationCenter,
									InOutFrameMeshData.VelocityQuantizationExtents);

			break;
		}

		case VelocityFormat::None:
		default:
		{
			break;
		}

	}	

}


//-----------------------------------------------------------------------------
// Converter::PackTexCoords
//-----------------------------------------------------------------------------
void Converter::PackTexCoords(std::vector<Vector2>& InTexCoords, std::vector<byte>& OutPackedData)
{
	if (InTexCoords.empty() || this->Options.TexCoordFormat_ == TexCoordFormat::None)
	{
		return;
	}

	switch (this->Options.TexCoordFormat_)
	{
		case TexCoordFormat::Full:
		{
			uint32 uSize = (uint32)(InTexCoords.size() * 2 * sizeof(float));

			OutPackedData.resize(uSize);
			memcpy(OutPackedData.data(), InTexCoords.data(), uSize);

			break;
		}

		case TexCoordFormat::Half:
		{
			uint32 uSize = (uint32)(InTexCoords.size() * 2 * sizeof(uint16));
			OutPackedData.resize(uSize);

			float* pInPositions = (float*)InTexCoords.data();
			uint16* pOutPositions = (uint16*)OutPackedData.data();
			// convert all floats (from range 0.0f to 1.0f) into uint16's ranging from 0 to 65535
			for (uint32 i = 0; i < InTexCoords.size() * 2; i++)
			{
				*pOutPositions++ = UnitFloatToUnsignedInt16(*pInPositions++);
			}

			break;
		}

		case TexCoordFormat::None:
		default:
		{
			break;
		}
	}	
}


//-----------------------------------------------------------------------------
// Converter::PackColors
//-----------------------------------------------------------------------------
void Converter::PackColors(std::vector<Vector4>& InColors, std::vector<byte>& OutPackedData, Vector4& OutColorQuantExtents)
{

	if (InColors.empty() || this->Options.ColorFormat_ == ColorFormat::None)
	{
		OutColorQuantExtents = Vector4(1.0f, 1.0f, 1.0f, 0.0f);
		return;
	}

	switch (this->Options.ColorFormat_)
	{
		case ColorFormat::Full:
		{
			uint32 uSize = (uint32)(InColors.size() * 4 * sizeof(float));

			OutPackedData.resize(uSize);
			memcpy(OutPackedData.data(), InColors.data(), uSize);

			OutColorQuantExtents = Vector4(1.0f, 1.0f, 1.0f, 1.0f);

			break;
		}

		case ColorFormat::Half:
		{
			QuantizeColorsToUInt16(InColors, OutPackedData, OutColorQuantExtents);
			break;
		}

		case ColorFormat::ByteHDR:
		{
			QuantizeColorsToUInt8(InColors, OutPackedData, OutColorQuantExtents);
			break;
		}

		case ColorFormat::Byte:
		{
			uint32 uSize = (uint32)(InColors.size() * 4 * sizeof(uint8));
			OutPackedData.resize(uSize);

			float* pInColors = (float*)InColors.data();
			uint8* pOutColors = (uint8*)OutPackedData.data();
			for (uint32 i = 0; i < InColors.size() * 4; i++)
			{
				// convert all colors to the 0 ... 255 range
				int32 c = (int32) (*pInColors++ * 255.0f);
				if (c < 0) c = 0;
				if (c > 255) c = 255;
				*pOutColors++ = (uint8) c;
			}

			OutColorQuantExtents = Vector4(1.0f, 1.0f, 1.0f, 1.0f);

			break;
		}

		case ColorFormat::None:
		default:
		{
			break;
		}

	}

}


std::wstring StrToWStr(std::string& InString)
{

	const char* c = InString.c_str();
	const size_t cSize = strlen(c) + 1;
	wchar_t* wc = new wchar_t[cSize];
	mbstowcs(wc, c, cSize);

	std::wstring w(wc);

	delete[] wc;

	return w;
}


//-----------------------------------------------------------------------------
// Converter::GenerateFrameImageData
//-----------------------------------------------------------------------------
void Converter::GenerateFrameImageData(InputImageSequence& InImageSequence, int InFrameIndex, FrameImageData& InOutImageData)
{
#ifdef SUPPORT_IMAGE_SEQUENCES	
	// 
	std::string nameOfFileToConvert;
	if (InFrameIndex >= InImageSequence.Files.size())	
	{
		// SPECIAL CASE: always convert the first frame of image sequences that contain only a single frame (constant)
		if ((InFrameIndex == this->StartFrame && InImageSequence.Files.size() == 1))
		{
			nameOfFileToConvert = InImageSequence.Files[0];
		}
		else
		{
			// out of bound, ignore this image sequence for this frame
			return;
		}
	}
	else
	{
		nameOfFileToConvert = InImageSequence.Files[InFrameIndex];
	}


	wchar_t arg0[64];
	wcscpy(arg0, L"abcToKimura");

	wchar_t arg1[64];
	switch (InImageSequence.Format)
	{
		case ImageFormat::DXT1:
		{
			wcscpy(arg1, L"-f:DXT1");
			break;
		}

		case ImageFormat::DXT3:
		{
			wcscpy(arg1, L"-f:DXT3");
			break;
		}

		case ImageFormat::DXT5:
		{
			wcscpy(arg1, L"-f:DXT5");
			break;
		}

		default:
		{
			wcscpy(arg1, L"-f:DXT1");
			break;
		}
	}


	std::wstring tmp = StrToWStr(nameOfFileToConvert);
	wchar_t arg2[512];
	wcscpy(arg2, tmp.c_str());

	// make sure that the final textures are saved in power of two
	wchar_t arg3[32] = L"-pow2";
	//wchar_t arg4[32] = L"-fixbc4x4";

	int numTexConvArguments = 4;

	wchar_t* texConvArguments[] = { arg0, arg1, arg2, arg3 };
	std::vector<Kimura::Mipmap> mips;

	// this can only be called when image sequences are supported
	texconv(numTexConvArguments, texConvArguments, mips);

	InOutImageData.NumMipmaps = 0;
	for (int i = 0; i < (int)mips.size(); i++)
	{
		if (mips[i].width <= InImageSequence.MaxSize && InOutImageData.NumMipmaps < MaxMipmaps)
		{
			InOutImageData.Mipmaps[InOutImageData.NumMipmaps].Data = mips[i].buffer;
			InOutImageData.Mipmaps[InOutImageData.NumMipmaps].DataHash = ArrayHash<byte>((byte*)mips[i].buffer.data(), (int)mips[i].buffer.size());
			InOutImageData.Mipmaps[InOutImageData.NumMipmaps].Width = mips[i].width;
			InOutImageData.Mipmaps[InOutImageData.NumMipmaps].Height = mips[i].height;
			InOutImageData.Mipmaps[InOutImageData.NumMipmaps].RowPitch = mips[i].rowPitch;
			InOutImageData.Mipmaps[InOutImageData.NumMipmaps].SlicePitch = mips[i].slicePitch;

			InOutImageData.NumMipmaps++;

		}


	}
#endif
}


//-----------------------------------------------------------------------------
// TryParseArgument
//-----------------------------------------------------------------------------
inline std::string TryParseArgument(std::string& InStringToTest, std::string&& InStartsWith)
{
	if (InStringToTest.find(InStartsWith) == 0)
	{
		return InStringToTest.substr(InStartsWith.length());
	}

	return "";
}


//-----------------------------------------------------------------------------
// ConverterOptions::Parse
//-----------------------------------------------------------------------------
bool ConverterOptions::Parse(std::vector<std::string>& InArgs)
{
	try 
	{
		for (std::string& argument : InArgs)
		{
			std::string help = TryParseArgument(argument, "help");
			std::string input = TryParseArgument(argument, "i:");
			std::string output = TryParseArgument(argument, "o:");
			std::string opt = TryParseArgument(argument, "opt:");
			std::string splitMeshes = TryParseArgument(argument, "split:");
			std::string scale = TryParseArgument(argument, "scale:");
			std::string startFrame = TryParseArgument(argument, "start:");
			std::string endFrame = TryParseArgument(argument, "end:");
			std::string posFormat = TryParseArgument(argument, "pFmt:");
			std::string normalFormat = TryParseArgument(argument, "nFmt:");
			std::string tangentFormat = TryParseArgument(argument, "ntFmt:");
			std::string velocityFormat = TryParseArgument(argument, "vFmt:");
			std::string texcoordFormat = TryParseArgument(argument, "tFmt:");
			std::string colorFormat = TryParseArgument(argument, "cFmt:");
			std::string swizzle = TryParseArgument(argument, "swizzle:");
			std::string flipOrder = TryParseArgument(argument, "flip:");
			std::string flipUV = TryParseArgument(argument, "flipUV:");
			std::string preset = TryParseArgument(argument, "preset:");
			std::string savePreset = TryParseArgument(argument, "bind:");
			std::string cpu = TryParseArgument(argument, "cpu:");

			// image sequence options
			for (int i = 0; i < MaxImageSequences; i++)
			{
				char tmp[256];

				// path
				sprintf(tmp, "image%d:", i);
				std::string path = TryParseArgument(argument, tmp);
				if (!path.empty())
				{
					std::filesystem::path p = path;
					std::filesystem::path completePath = std::filesystem::absolute(p);

					ImageSequences[i].Path = completePath.string();
				}
				else
				{
					// desired format
					sprintf(tmp, "image%dfmt:", i);
					std::string fmt = TryParseArgument(argument, tmp);
					if (!fmt.empty())
					{
						ImageSequences[i].Format = fmt;
					}
					else
					{
						// generate mipmaps
						sprintf(tmp, "image%dmips:", i);
						std::string mips = TryParseArgument(argument, tmp);
						if (!mips.empty())
						{
							ImageSequences[i].Mipmaps = mips == "true";
						}
						else
						{
							// max size
							sprintf(tmp, "image%dsize:", i);
							std::string size = TryParseArgument(argument, tmp);
							if (!size.empty())
							{
								ImageSequences[i].MaxSize = stoi(size);
							}

						}

					}

				}

			}

			if (!help.empty())
			{
				return false;
			}
			if (!input.empty())
			{
				std::filesystem::path p = input;
				std::filesystem::path completePath = std::filesystem::absolute(p);

				this->SourceFile = completePath.string();
			}
			else if (!output.empty())
			{
				std::filesystem::path p = output;
				std::filesystem::path completePath = std::filesystem::absolute(p);

				this->DestinationFile = completePath.string();
			}
			else if (!opt.empty())
			{
				this->MeshOptimization = opt == "true";
			}
			else if (!splitMeshes.empty())
			{
				this->Force16bitIndices = splitMeshes == "true";
			}
			else if (!scale.empty())
			{
				this->Scale = stof(scale);
				if (scale != scale)
				{
					return false;
				}
			}
			else if (!startFrame.empty())
			{
				this->StartFrame = stoi(startFrame);
				if (this->StartFrame >= this->EndFrame)
				{
					return false;
				}
			}
			else if (!endFrame.empty())
			{
				this->EndFrame = stoi(endFrame);
				if (this->StartFrame >= this->EndFrame)
				{
					return false;
				}
			}
			else if (!posFormat.empty())
			{
				if (posFormat == "full")
				{
					this->PositionFormat_ = PositionFormat::Full;
				}
				else if (posFormat == "half")
				{
					this->PositionFormat_ = PositionFormat::Half;
				}
			}
			else if (!normalFormat.empty())
			{
				if (normalFormat == "full")
				{
					this->NormalFormat_ = NormalFormat::Full;
				}
				else if (normalFormat == "half")
				{
					this->NormalFormat_ = NormalFormat::Half;
				}
				else if (normalFormat == "byte")
				{
					this->NormalFormat_ = NormalFormat::Byte;
				}
				else if (normalFormat == "none")
				{
					this->NormalFormat_ = NormalFormat::None;
				}
			}
			else if (!tangentFormat.empty())
			{
				if (tangentFormat == "full")
				{
					this->TangentFormat_ = TangentFormat::Full;
				}
				else if (tangentFormat == "half")
				{
					this->TangentFormat_ = TangentFormat::Half;
				}
				else if (tangentFormat == "byte")
				{
					this->TangentFormat_ = TangentFormat::Byte;
				}
				else if (tangentFormat == "none")
				{
					this->TangentFormat_ = TangentFormat::None;
				}

			}
			else if (!velocityFormat.empty())
			{
				if (velocityFormat == "full")
				{
					this->VelocityFormat_ = VelocityFormat::Full;
				}
				else if (velocityFormat == "half")
				{
					this->VelocityFormat_ = VelocityFormat::Half;
				}
				else if (velocityFormat == "byte")
				{
					this->VelocityFormat_ = VelocityFormat::Byte;
				}
				else if (velocityFormat == "none")
				{
					this->VelocityFormat_ = VelocityFormat::None;
				}
			}
			else if (!texcoordFormat.empty())
			{
				if (texcoordFormat == "full")
				{
					this->TexCoordFormat_ = TexCoordFormat::Full;
				}
				else if (texcoordFormat == "half")
				{
					this->TexCoordFormat_ = TexCoordFormat::Half;
				}
				else if (texcoordFormat == "none")
				{
					this->TexCoordFormat_ = TexCoordFormat::None;
				}
			}
			else if (!colorFormat.empty())
			{
				if (colorFormat == "full")
				{
					this->ColorFormat_ = ColorFormat::Full;
				}
				else if (colorFormat == "half")
				{
					this->ColorFormat_ = ColorFormat::Half;
				}
				else if (colorFormat == "byte")
				{
					this->ColorFormat_ = ColorFormat::Byte;
				}
				else if (colorFormat == "bytehdr")
				{
					this->ColorFormat_ = ColorFormat::ByteHDR;
				}
				else if (colorFormat == "none")
				{
					this->ColorFormat_ = ColorFormat::None;
				}
			}
			else if (!swizzle.empty())
			{
				if (swizzle == "yz")
				{
					this->Swizzle_ = Swizzle::YZ;
				}
				else if (swizzle == "xz")
				{
					this->Swizzle_ = Swizzle::XZ;
				}
			}
			else if (!flipOrder.empty())
			{
				this->FlipIndiceOrder = flipOrder == "true";
			}
			else if (!flipUV.empty())
			{
				this->FlipTextureCoords = flipUV == "true";
			}
			else if (!cpu.empty())
			{
				this->NumThreadUsedForProcessingFrames = stoi(cpu);

				if (this->NumThreadUsedForProcessingFrames == 0)
				{
					std::printf("Invalid argument for 'cpu'\n");
					return false;
				}

			}
			else if (!preset.empty())
			{
				if (preset == "ue4")
				{
					
				}
			}

		}

	}
	catch (const std::exception&)
	{
		std::printf("Error while parsing arguments\n");
		return false;
	}

	return true;

}
