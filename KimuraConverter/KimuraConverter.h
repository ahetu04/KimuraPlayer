//
// Copyright (c) Alexandre Hetu.
// Licensed under the MIT License.
//
// https://github.com/ahetu04
//

#pragma once

#include "../Player/Include/Kimura.h"
#include "../Player/Player.h"
#include "Include/IKimuraConverter.h"

#include <string>
#include <vector>
#include <thread>
#include <fstream>

#include "Alembic/AbcCoreFactory/IFactory.h"
#include "Alembic/Abc/IArchive.h"
#include "Alembic/Abc/IObject.h"
#include "Alembic/AbcGeom/All.h"
#include "Threadpool/Threadpool.h"


namespace Kimura
{

	template <typename T>
	inline size_t ArrayHash(T* v, int count)
	{
		size_t seed = 0;
		for (int i = 0; i < count; i++)
		{
			seed ^= std::hash<T>()(*v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
			v++;
		}
		return seed;
	}

	template<typename T>
	inline size_t StdVectorHash(std::vector<T>& InVector)
	{
		return ArrayHash<T>(InVector.data(), (int)InVector.size());
	}

	inline float fAbs(float f)
	{
		return f < 0.0f ? -f : f;
	}

	inline bool fEquals(float a, float b, float threshold)
	{
		float absolute = fAbs(a-b);

		return absolute < threshold;
	}


	inline bool vEqual(const Vector2& a, const Vector2& b, float threshold = 0.0001f)
	{
		return fEquals(a.X, b.X, threshold) && fEquals(b.Y, b.Y, threshold);
	}

	inline bool vEqual(const Vector3& a, const Vector3& b, float threshold = 0.0001f)
	{
		return fEquals(a.X, b.X, threshold) && fEquals(a.Y, b.Y, threshold) && fEquals(a.Z, b.Z, threshold);
	}

	inline bool vEqual(const Vector4& a, const Vector4& b, float threshold = 0.0001f)
	{
		return fEquals(a.X, b.X, threshold) && fEquals(a.Y, b.Y, threshold) && fEquals(a.Z, b.Z, threshold) && fEquals(a.W, b.W, threshold);
	}


	inline uint16 UnitFloatToUnsignedInt16(float f32)
	{
		float f = f32 * 65535.0f;
		int32	_toInt = (int)f;
		if (_toInt < 0)
			_toInt = 0;
		if (_toInt > 65535)
			_toInt = 65535;

		return (uint16)_toInt;
	}


	inline int16 UnitFloatToInt16(float f32)
	{
		float f = f32 * 32767.5f;
		int32	_toInt = (int)f;
		if (_toInt < -32767)
			_toInt = -32767;
		if (_toInt > 32676)
			_toInt = 32767;

		return (int16)_toInt;
	}


	inline int8 UnitFloatToInt8(float f32)
	{
		float f = f32 * 127.5f;
		int32	_toInt = (int)f;
		if (_toInt < -127)
			_toInt = -127;
		if (_toInt > 127)
			_toInt = 127;

		return (int8)_toInt;
	}


	inline int16 ClampInt16(float f)
	{
		int32 i = (int32)f;
		if (i < -32767) i = -32767;
		if (i > 32767) i = 32767;

		return (int16) i;
	}

	enum class Swizzle
	{
		None, 
		XZ,
		YZ
	};

	class ConverterOptions
	{

		public:

			struct ImageSequenceOptions
			{
				std::string Path;
				std::string Format;
				int			MaxSize = 8192;
				bool		Mipmaps = true;

			};

			bool Parse(std::vector<std::string>& InArgs);

			std::string			SourceFile;
			std::string			DestinationFile;

			float				Scale = 1.0f;

			int					StartFrame = 0;
			int					EndFrame = 9999999;

			bool				MeshOptimization = true;
			bool				Force16bitIndices = true;

			PositionFormat		PositionFormat_ = PositionFormat::Full;
			NormalFormat		NormalFormat_ = NormalFormat::Half;
			TangentFormat		TangentFormat_ = TangentFormat::Half;
			VelocityFormat		VelocityFormat_ = VelocityFormat::Byte;
			TexCoordFormat		TexCoordFormat_ = TexCoordFormat::Half;
			ColorFormat			ColorFormat_ = ColorFormat::Byte; 

			Swizzle				Swizzle_ = Swizzle::None;
			bool				FlipIndiceOrder = false;
			bool				FlipTextureCoords = true;

			bool				TriangleStrip = false;

			int					NumThreadUsedForProcessingFrames = -1;

			bool				Verbose = true;

			static const int		MaxImageSequences = 16;
			ImageSequenceOptions	ImageSequences[MaxImageSequences];

	};


	class ConverterCacheData
	{
		public:
			std::map<std::string, size_t>	TextureLastAccessTimes;	
	};


	enum class Warnings : int
	{
		InsufficentDataToGenerateTangents = 0, 
		MissingNormals,
		MissingTangents,
		MissingVelocity,
		MissingTexCoords,
		MissingColors, 
		PolygonConversionRequired,
		InvalidPolygonsDetected,
		Count
	};

	std::map<Warnings, std::string> WarningMap
	{
		{Warnings::InsufficentDataToGenerateTangents, "Warning: Trying to generate tangents but missing normals and/or texture coordinates to do so."},
		{Warnings::MissingNormals, "Warning: Failed to produce desired vertex normal data. Overriding format to NormalFormat::None."},
		{Warnings::MissingTangents, "Warning: Failed to produce desired vertex tangents. Overriding format to TangentFormat::None."},
		{Warnings::MissingVelocity, "Warning: Failed to produce desired vertex velocity data. Overriding format to VelocityFormat::None."},
		{Warnings::MissingTexCoords, "Warning: Failed to produce desired texture coordinates. Overriding format to TexCoordFormat::None."},
		{Warnings::MissingTexCoords, "Warning: Failed to produce desired colors. Overriding format to ColorFormat::None."},
		{Warnings::PolygonConversionRequired, "Warning: Polygon conversion to triangles was necessary. Try to provide triangulated meshes."},
		{Warnings::InvalidPolygonsDetected, "Warning: A mesh containing invalid polygons (more than 4 points) was detected."}
	};


	class Converter : public IKimuraConverter
	{
		public:


			Converter(const ConverterOptions& InOptions) 
			{
				this->Options = InOptions;
			};

			virtual void Start() override;
			virtual void Stop() override;

			virtual volatile bool IsWorking() override
			{
				return !this->Error && !this->Done;
			}

			virtual bool HasSucceeded() override
			{
				return !this->Error && this->Done;
			}

			virtual std::string GetErrorMessage() override
			{
				return this->ErrorMessage;
			}

			virtual void GetConversionProgress(int& OutNumFramesWritten, int& OutNumFramesTotal) override
			{
				OutNumFramesWritten = this->NumFramesSaved;
				OutNumFramesTotal = this->NumFrames;
			}

			static void PrintHelp();

		protected:

			friend class FrameProcessingTask;

			struct AbcArchiveMesh
			{
				std::string Name;
				
				int			StartFrame;
				int			EndFrame;
				int			NumberOfFrames;

				Alembic::Abc::IObject AbcObject;

				long		MaximumVertices = 0;
				long		MaximumSurfaces = 0;

				bool		HasNormals = false;
				bool		HasTangents = false;
				bool		HasVelocity = false;
				bool		HasTexCoords = false;
				bool		HasColors = false;


			};


			class InputImageSequence
			{
				public:

					std::string Name;

					std::string Path;

					std::vector<std::string> Files;

					ImageFormat	Format = ImageFormat::DXT1;

					int MaxSize = 8192;

					bool Mipmaps = true;

			};

			class FrameMeshData
			{

				public:

					struct Section
					{
						unsigned int VertexStart = 0;		// offset in the vertex buffer
						unsigned int IndexStart = 0;		// offset in the index buffer
						unsigned int NumSurfaces = 0;
						unsigned int MinVertexIndex = 0;
						unsigned int MaxVertexIndex = 0;
					};

				public:
					FrameMeshData()
					{}

					unsigned int						Surfaces = 0;

					std::vector<unsigned int>			Indices;
					size_t								IndicesHash = 0;
					std::vector<byte>					IndicesPacked;

					std::vector<Kimura::Vector3>		Positions;
					size_t								PositionsHash = 0;
					std::vector<byte>					PositionsPacked;
					Vector3								PositionQuantizationCenter;
					Vector3								PositionQuantizationExtents;

					std::vector<Kimura::Vector3>		Normals;
					size_t								NormalsHash = 0;
					std::vector<byte>					NormalsPacked;

					std::vector<Kimura::Vector4>		Tangents;
					size_t								TangentsHash = 0;
					std::vector<byte>					TangentsPacked;

					std::vector<Kimura::Vector3>		Velocities;
					size_t								VelocitiesHash = 0;
					std::vector<byte>					VelocitiesPacked;
					Vector3								VelocityQuantizationCenter;
					Vector3								VelocityQuantizationExtents;

					int									UVCount = 0;
					std::vector<Kimura::Vector2>		UVChannels[MaxTextureCoords];
					size_t								UVChannelsHash[MaxTextureCoords] { 0, 0, 0, 0 };
					std::vector<byte>					UVChannelsPacked[MaxTextureCoords];

					int									ColorCount = 0;
					std::vector<Kimura::Vector4>		Colors[MaxColorChannels];
					size_t								ColorsHash[MaxColorChannels] { 0, 0 };
					std::vector<byte>					ColorsPacked[MaxColorChannels];
					Vector4								ColorQuantizationExtents[MaxColorChannels];


					Kimura::Vector3						BoundingCenter;
					Kimura::Vector3						BoundingSize;

					bool								Force16bitIndices = false;
					std::vector<Section>				Sections;

			};

			class MipmapData
			{
				public:
					int					Width = 0;
					int					Height = 0;
					int					RowPitch = 0;
					int					SlicePitch = 0;

					std::vector<byte>	Data;
					size_t				DataHash = 0;

			};

			class FrameImageData
			{
				public:
					int				NumMipmaps = 0;
					MipmapData		Mipmaps[MaxMipmaps];
			};

			class Frame
			{
				public:
					int									FrameIndex = -1;
					std::vector<FrameMeshData>			Meshes;
					std::vector<FrameImageData>			Images;

					uint32								TotalVertices = 0;
					uint32								TotalSurfaces = 0;
			};

			class OptimizationVertex
			{

				public:

					Vector3 P;
					Vector3 N;
					Vector3 V;
				
					Vector2	TextureCoords[MaxTextureCoords];
					Vector4	Colors[MaxColorChannels];

					inline bool Equals(OptimizationVertex& other, float tolerance = 0.00001f) const
					{

						if (!vEqual(P, other.P, tolerance) ||
							!vEqual(N, other.N, tolerance) ||
							!vEqual(V, other.V, tolerance) ||
							!vEqual(TextureCoords[0], other.TextureCoords[0], 0.0001f) || 
							!vEqual(TextureCoords[1], other.TextureCoords[1], 0.0001f) ||
							!vEqual(TextureCoords[2], other.TextureCoords[2], 0.0001f) ||
							!vEqual(TextureCoords[3], other.TextureCoords[3], 0.0001f) ||
							!vEqual(Colors[0], other.Colors[0], 0.005f) ||
							!vEqual(Colors[1], other.Colors[1], 0.005f))
						{
							return false;
						}

						return true;
					}

			};

			struct OptimizationGraphNode
			{
				OptimizationVertex			Vertex;
				int							Index;
				OptimizationGraphNode*		leafs[32];

			};

		protected:


			void DoWorkFromMainWorkThread();
			void FatalError(std::string InErrorMessage);

			void TravelHierarchyToFindMeshes(Alembic::Abc::IObject& InObject);
			void AddMeshFromIPolyMesh(Alembic::Abc::IObject& InObject);
			void AddMeshFromISubD(Alembic::Abc::IObject& InObject);

			void DiscoverImageSequence(ConverterOptions::ImageSequenceOptions& InImageSequenceOptions);

			void WriteTableOfContent();

			void ProcessAndSaveAllTheFrames();

			void ProcessFrame(int InFrameIndex, int InFrameProcessIndex);
			void GenerateFrameMeshData(AbcArchiveMesh& InMesh, int InFrameIndex, FrameMeshData& InOutMeshData);
			void GenerateTangentsOnFrameMesh(FrameMeshData& InOutMeshData);
			void OptimizeFrameMeshData(FrameMeshData& InOutMeshData);
			void PackFrameMeshData(FrameMeshData& InOutMeshData);
			void PackIndices(std::vector<uint32>& InIndices, std::vector<byte>& OutPackedData, bool InPack32bit);
			void PackPositions(std::vector<Vector3>& InPositions, std::vector<byte>& OutPackedData, FrameMeshData& InOutFrameMeshData);
			void PackNormals(std::vector<Vector3>& InNormals, std::vector<byte>& OutPackedData);
			void PackTangents(std::vector<Vector4>& InTangents, std::vector<byte>& OutPackedData);
			void PackVelocities(std::vector<Vector3>& InVelocities, std::vector<byte>& OutPackedData, FrameMeshData& InOutFrameMeshData);
			void PackTexCoords(std::vector<Vector2>& InTexCoords, std::vector<byte>& OutPackedData);
			void PackColors(std::vector<Vector4>& InColors, std::vector<byte>& OutPackedData, Vector4& OutColorQuantExtents);


			void GenerateFrameImageData(InputImageSequence& InImageSequence, int InFrameIndex, FrameImageData& InOutImageData);

			void UpdateTOCAndWriteFrameToDisk(std::shared_ptr<Frame> InFrameToSave);

			// bunch of functions to generate raw mesh data from the alembic archive
			bool PopulateRawMeshDataFromPolyMeshSchema(Alembic::Abc::IObject InObject, const Alembic::AbcGeom::IPolyMeshSchema* InPolyMeshSchema, const Alembic::AbcGeom::ISubDSchema* InSubDSchema, const Alembic::Abc::ISampleSelector InFrameSelector, FrameMeshData& InRawMeshFrameData, const bool InForceFirstFrame);
			template<typename T, typename U> bool CopyAbcElementsToKimuraElements(T InSampleDataPtr, std::vector<U>& OutData);
			template<typename T> void TriangulateBuffer(const std::vector<unsigned int>& InNumIndicesPerSurface, std::vector<T>& InOutIndexBufferToTriangulate);
			template<typename T> void ConvertToNonIndexedElements(const std::vector<unsigned int>& InIndexBuffer, std::vector<T>& InOutElements);
			template <typename abcParamType, typename abcElementType, typename kimuraElementType>
			void ExtractElementsFromGeomParam(const Alembic::Abc::ISampleSelector InFrameSelector, abcParamType p, std::vector<kimuraElementType>& outputData, std::vector<unsigned int>& indiceCountPerSurface, std::vector<unsigned int>& defaultIndices, int defaultNumElements, bool meshHasQuads);


			template<typename T>
			uint32 Write(T& In, uint32 InCount = 1);
			uint32 Write(std::string& s);
			uint32 Write(std::vector<byte>& d);

			inline void RaiseWarning(Warnings w)
			{
				if (!this->RaisedWarnings[(int)w])
				{
					this->RaisedWarnings[(int)w] = true;
				}
			}


			ConverterOptions						Options;

			std::thread*							MainWorkThread = nullptr;

			Alembic::AbcCoreFactory::IFactory		AbcFactory;
			Alembic::Abc::IArchive					AbcArchive;
			Alembic::Abc::IObject					AbcRootObject;

			std::ofstream							OutputFile;

			uint64									CurrentFrameOffset = 0;

			float									TimePerFrame = 1.0f / 30.0f;
			float									FrameRate = 30.0f;
			int										NumFrames = 0;

			std::vector<AbcArchiveMesh>				Meshes;
			int										StartFrame = 0;
			int										EndFrame = 0;


			std::vector<InputImageSequence>			ImageSequences;

			std::mutex								FrameProcessingMutex;
			std::vector<std::shared_ptr<Frame>>		Frames;
			int										IndexOfNextFrameToWrite = 0;
			int										IndexOfLastFrameQueuedForProcessing = 0;
			int										NumFramesSaved = 0;

			std::shared_ptr<Frame>					LastFrameSaved = nullptr;

			TableOfContent							TOC;

			volatile bool							Canceled = false;
			volatile bool							Done = false;
			volatile bool							Error = false;
			std::string								ErrorMessage;

			volatile float							PercentageComplete = 0.0f;

			bool									TangentWarningRaised = false;

			volatile bool							RaisedWarnings[(int)Warnings::Count];

	};

}
