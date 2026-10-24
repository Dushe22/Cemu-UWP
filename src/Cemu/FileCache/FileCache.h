#pragma once

#include <condition_variable>
#include <mutex>
#include <unordered_map>

struct _FileCacheAsyncWriter;

class FileCache
{
public:
	struct FileName 
	{
		FileName(uint64 name1, uint64 name2) : name1(name1), name2(name2) {};
		FileName(std::string_view filePath)
		{
			// name from string hash
			uint64 h1 = 0xa2cc2c49386a75fdull;
			uint64 h2 = 0x5182d367734c2ce8ull;
			const char* c = filePath.data();
			const char* cEnd = filePath.data() + filePath.size();
			while (c < cEnd)
			{
				uint64 t = (uint64)*c;
				c++;
				h1 = (h1 << 7) | (h1 >> (64 - 7));
				h1 += t;
				h2 = h2 * 7841u + t;
			}
			name1 = h1;
			name2 = h2;
		};

		FileName(const std::string& filePath) : FileName(std::basic_string_view(filePath.data(), filePath.size())) {};

		uint64 name1;
		uint64 name2;
	};

	~FileCache();

	static FileCache* Create(const fs::path& path, uint32 extraVersion = 0);
	static FileCache* Open(const fs::path& path, bool allowCreate, uint32 extraVersion = 0);
	static FileCache* Open(const fs::path& path); // open without extraVersion check

	void UseCompression(bool enable) { enableCompression = enable; };

	void AddFile(const FileName&& name, const uint8* fileData, sint32 fileSize);
	void AddFileAsync(const FileName& name, const uint8* fileData, sint32 fileSize);
	bool DeleteFile(const FileName&& name);
	bool GetFile(const FileName&& name, std::vector<uint8>& dataOut);
	bool GetFileByIndex(sint32 index, uint64* name1, uint64* name2, std::vector<uint8>& dataOut);
	bool GetUncompressedFileByIndexBounded(sint32 index, uint64* name1, uint64* name2, std::vector<uint8>& dataOut, size_t maxBytes);
	bool GetUncompressedFileInfoByIndexBounded(sint32 index, uint64& name1, uint64& name2, uint32& size, size_t maxBytes);
	bool HasFile(const FileName&& name);
	// names of all stored files (name1, name2), for bulk membership checks
	void GetFileNames(std::vector<std::pair<uint64, uint64>>& namesOut);

	sint32 GetFileCount();

	sint32 GetMaximumFileIndex();

private:
	friend struct _FileCacheAsyncWriter;

	struct FileTableEntry
	{
		enum FLAGS : uint8
		{
			FLAG_NONE = 0x00,
			FLAG_COMPRESSED = (1 << 0), // zLib compressed
		};
		uint64 name1;
		uint64 name2;
		uint64 fileOffset;
		uint32 fileSize;
		FLAGS flags;
		uint8 extraReserved1;
		uint8 extraReserved2;
		uint8 extraReserved3;
	};

	static_assert(sizeof(FileTableEntry) == 0x20);

	FileCache() {};

	static FileCache* _OpenExisting(const fs::path& path, bool compareExtraVersion, uint32 extraVersion = 0);

	void fileCache_updateFiletable(sint32 extraEntriesToAllocate);
	void _addFileInternal(uint64 name1, uint64 name2, const uint8* fileData, sint32 fileSize, bool noCompression);
	bool _getFileDataInternal(const FileTableEntry* entry, std::vector<uint8>& dataOut);
	void asyncWriteFinished();
	// name -> table index, so lookups do not scan the whole table (callers hold mutex)
	void _rebuildNameIndex();
	sint32 _findEntryIndex(uint64 name1, uint64 name2) const;

	class FileStream* fileStream{};
	uint64 dataOffset{};
	uint32 extraVersion{};
	// file table
	FileTableEntry* fileTableEntries{};
	sint32 fileTableEntryCount{};
	// file table (as stored in file)
	uint64 fileTableOffset{};
	uint32 fileTableSize{};
	// options
	bool enableCompression{true};
	struct NameHash
	{
		size_t operator()(const std::pair<uint64, uint64>& name) const { return (size_t)(name.first ^ (name.second * 0x9E3779B97F4A7C15ull)); }
	};
	std::unordered_map<std::pair<uint64, uint64>, sint32, NameHash> nameIndex;
	sint32 freeSearchStart{};

	std::recursive_mutex mutex;
	std::mutex asyncWriteMutex;
	std::condition_variable asyncWriteCondition;
	uint32 asyncWritesPending{};
	bool acceptingAsyncWrites{true};
};
