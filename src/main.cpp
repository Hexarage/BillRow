// challenge expects the entire program to be jammed into one file

#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <immintrin.h>
#include <thread>
#include <new>
#include <unordered_map>
#include <cstring>
#include <csignal>
#include <atomic>
#include <malloc.h>

constexpr uint32_t SMALL = 749449;
constexpr uint32_t SHL_CONST = 18;
constexpr uint32_t BIN_COUNT = 16384 * 8; // In the 10k key cases, for 413 cases the  * 8 is not needed
constexpr int MAX_KEY_LENGTH = 100;

#ifndef THREAD_COUNT_DEFAULT
constexpr int MAX_N_THREADS = 8; // in order to match the evaluation server (if I ever decide to submit this)
#else
constexpr int MAX_N_THREADS = THREAD_COUNT_DEFAULT;
#endif

#ifndef N_CORES_PARAM
constexpr int N_CORES = MAX_N_THREADS;
#else
constexpr int N_CORES = N_CORES_PARAM;
#endif

constexpr bool DEBUG = 0;

class MyTimer 
{
public:
	void startCounter(){
		start = std::chrono::system_clock::now();
	}
	
	int64_t getCounterNs()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now() - start).count();
	}

	int64_t getCounterMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start).count();
	}

	double getCounterMsPrecise()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now() - start).count() / 1000000.0;
	}

private:
	std::chrono::time_point<std::chrono::system_clock> start;
};

struct Stats
{
	int64_t sum;
	int count;
	int max;
	int min;

	Stats()
	: count(0),
	sum(0),
	max(-1024),
	min(1024)
	{}

	bool operator<(const Stats& other)
	{
		return min < other.min;
	}
};

struct HashBin
{
	uint8_t keyShort[32]; // to make use of short string optimization
	int64_t sum;
	int count;
	int max;
	int min;
	int length;
	uint8_t *keyLong;

	HashBin()
	{
		length = 0;
		memset(keyShort, 0, sizeof(keyShort));
		keyLong = nullptr;
	}
};
static_assert(sizeof(HashBin) == 64); // Array indexing happens faster if the struct is a power of in size

std::unordered_map<std::string, Stats> partialStats[MAX_N_THREADS];
std::unordered_map<std::string, Stats> finalStats;

HashBin *globalHMaps;
alignas(4096) HashBin *hMaps[MAX_N_THREADS];

alignas(4096) uint8_t strcmpMask32[64] = {
	255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0
};

inline int __attribute__((always_inline)) mm128i_equal(__m128i a, __m128i b)
{
	__m128i neq = _mm_xor_si128(a, b); // bitwise XOR of a and b and store in dst
	return _mm_test_all_zeros(neq, neq); // bitwise AND of 128 bits and set the dst to 1 if all bits are 0
}

inline int __attribute__((always_inline)) mm256i_equal(__m256i a, __m256i b)
{
	__m256i neq = _mm256_xor_si256(a, b); // 256 bit bitwise XOR  
	return _mm256_testz_si256(neq, neq); // 256 bit bitwise AND, set dst to 1 if all bits are 0
}

inline void __attribute__((always_inline)) handleLineSlow(const uint8_t *data, HashBin *hmap, size_t &dataIndex)
{
	uint32_t pos = 16;
	uint32_t ownHash;

	__m128i characters = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data)); // load integer data into dst
	__m128i index  = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); // set supplied packed values into dst in reverse order
	__m128i separators = _mm_set1_epi8(';'); // broadcast 1 byte int into all elements of dst
	__m128i compared = _mm_cmpeq_epi8(characters, separators); // compare packed ints in a and b for equality and store result in dst

	uint32_t separatorMask = _mm_movemask_epi8(compared); // Create mask from the most significant bit of each 8-bit element in a and store in dst.

	if(separatorMask)[[likely]]
	{
		pos = __builtin_ctz(separatorMask); // gcc function that counts trailing zeroes
	}

	// sum the 2 16 char halves together and then hash the resulting 8 chars
	// saves a _mm256_mullo_epi32 instruction
	// __m128i mask == _mm_loadu_si128((__m128i*)(strncmp_mask + 16 - pos))
	__m128i mask = _mm_cmplt_epi8(index, _mm_set1_epi8(pos)); // compare a and b for less than and output in dst
	__m128i keyCharacters = _mm_and_si128(characters, mask); // 128bit bitwise AND
	__m128i sumCharacters = _mm_add_epi8(keyCharacters, _mm_unpackhi_epi64(keyCharacters, keyCharacters)); // unpack and interleave the ints from high half of a and b
	// add packed 8 bit ints from a and b and store in dst

	ownHash = (static_cast<uint64_t>(_mm_cvtsi128_si64(sumCharacters)) * SMALL) >> SHL_CONST; // copy the lower 64 bit int to dst

	if(data[pos]!= ';') [[unlikely]]
	{
		__m256i characters32_1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 17)); // load 256 bits into dst
		__m256i characters32_2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 17 + 32));
		__m256i separators32 = _mm256_set1_epi8(';'); // broadcast byte int into all elements of dst

		__m256i compared32_1 = _mm256_cmpeq_epi8(characters32_1, separators32);
		__m256i compared32_2 = _mm256_cmpeq_epi8(characters32_2, separators32);
		
		uint64_t separatorMask64 = static_cast<uint64_t>(_mm256_movemask_epi8(compared32_1)) | (static_cast<uint64_t>(_mm256_movemask_epi8(compared32_2)) << 32);

		if(separatorMask64) [[likely]]
		{
			pos = 17 + __builtin_ctzll(separatorMask64); // long long version of __builtin_ctz
		} else
		{
			__m256i characters32_3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 81));
			__m256i compared32_3 = _mm256_cmpeq_epi8(characters32_3, separators32);
			uint32_t separatorMaskFinal = _mm256_movemask_epi8(compared32_3);
			pos = 81 + __builtin_ctz(separatorMaskFinal);
		}
	}

	PARSE_VALUE:
	// must not move the hash table probe prior to value parsing
	// it slows down everything
	int len = pos;
	pos += (data[pos + 1] == '-'); // ensure that data[pos] is at the position directly prior to first digit
	int sign = (data[pos] == '-') ? -1 : 1;
	ownHash %= BIN_COUNT;

	// code based off of curiouscoding.nl; must use uint32_t otherwise there's UB
	uint32_t uuvalue;
	std::memcpy(&uuvalue, data + pos + 1, 4);
	uuvalue  <<= 8 * (data[pos+2] == '.');
	constexpr uint64_t C = 1 + (10 << 16) + (100 << 24);
	uuvalue &= 0x0f000f0f;
	uuvalue = ((uuvalue * C) >> 24) & ((1 << 10) - 1); // branchless
	int value = static_cast<int>(uuvalue) * sign;

	// put index updating prior to hmap insert to improve
	// register dependency chain
	dataIndex += pos + 5 + (data[pos + 3] == '.');

	if(len <= 16) [[likely]]
	{
		// loading everything and recalculating rather than using old result is consistently faster
		// TODO: Investigate why the damn
		__m128i characters = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
		__m128i index  = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
		__m128i mask = _mm_cmplt_epi8(index, _mm_set1_epi8(len));
		__m128i keyCharacters = _mm_and_si128(characters, mask);

		__m128i binCharacters = _mm_load_si128(reinterpret_cast<const __m128i*>(hmap[ownHash].keyShort));
		__m128i neq = _mm_xor_si128(binCharacters, keyCharacters);

		if(_mm_test_all_zeros(neq, neq) || hmap[ownHash].length == 0) [[likely]]
		{
			// checking first bin prior to loop consistently lowers total time
		} else
		{
			ownHash = (ownHash + 1) % BIN_COUNT;
			while (hmap[ownHash].length > 0)
			{
				// SIMD String comparison
				__m128i binCharacters = _mm_load_si128(reinterpret_cast<const __m128i*>(hmap[ownHash].keyShort));
				if(mm128i_equal(keyCharacters, binCharacters)) [[likely]]
				{
					break;
				}
				ownHash = (ownHash + 1) % BIN_COUNT;
			}
		}
	} else if(len <= 32  && hmap[ownHash].length != 0) [[likely]]
	{
		__m256i characters = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
		__m256i mask = _mm256_loadu_si256(reinterpret_cast<const __m256i*>((strcmpMask32 + 32 - len)));
		__m256i keyCharacters = _mm256_and_si256(characters, mask);

		__m256i binCharacters = _mm256_load_si256(reinterpret_cast<const __m256i*>(hmap[ownHash].keyShort));
		if(mm256i_equal(keyCharacters, binCharacters)) [[likely]]
		{
			// do nothing
		} else
		{
			ownHash = (ownHash + 1) % BIN_COUNT;
			while(hmap[ownHash].length > 0)
			{
				// another SIMD String comparison
				__m256i binCharacters = _mm256_load_si256(reinterpret_cast<const __m256i*>(hmap[ownHash].keyShort));
				if(mm256i_equal(keyCharacters, binCharacters)) [[likely]]
				{
					break;
				}
				ownHash = (ownHash + 1) % BIN_COUNT;
			}
		}
	} else
	{
		while(hmap[ownHash].length > 0)
		{
			if(hmap[ownHash].length == len) [[likely]]
			{
				int index = 0;
				while((index + 32) < len)
				{
					__m256i characters = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + index));
					__m256i binCharacters = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(hmap[ownHash].keyLong + index));
					if(!mm256i_equal(characters, binCharacters)) [[unlikely]]
					{
						goto NEXT_LOOP; // I don't like gotos but I can't find or figuree out a way that's better for this
					}
					index += 32;
				}

				if(index <= 64) [[likely]]
				{
					__m256i mask = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(strcmpMask32+32 - (len + index)));
					__m256i characters = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + index));
					__m256i keyCharacters = _mm256_and_si256(characters, mask);
					__m256i binCharacters = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(hmap[ownHash].keyLong + index));

					if(mm256i_equal(keyCharacters, binCharacters)) [[likely]]
					{
						break;
					}
				} else
				{
					bool equal = true;
					for (int i = index; i < len; ++i)
					{
						if(data[i] != hmap[ownHash].keyLong[i])
						{
							equal = false;
							break;
						}
					}
					if(equal) [[likely]]
					{
						break;
					}
				}
			}
			NEXT_LOOP:
			ownHash = (ownHash + 1) % BIN_COUNT;
		}
	}

	hmap[ownHash].count++;
	hmap[ownHash].sum += value;

	if(hmap[ownHash].max < value) [[unlikely]]
	{
		hmap[ownHash].max = value;
	}
	if(hmap[ownHash].min > value) [[unlikely]]
	{
		hmap[ownHash].min = value;
	}

	if(hmap[ownHash].length == 0) [[unlikely]] // unlikely because each key will be free once
	{
		hmap[ownHash].length = len;
		hmap[ownHash].sum = value;
		hmap[ownHash].count = 1;
		hmap[ownHash].max = value;
		hmap[ownHash].min = value;
		if(len < 32)
		{
			std::memcpy(hmap[ownHash].keyShort, data, len);
			std::memcpy(hmap[ownHash].keyShort + len, 0, 32 - len);
		} else
		{
			hmap[ownHash].keyLong = new uint8_t[MAX_KEY_LENGTH];
			std::memcpy(hmap[ownHash].keyLong, data, len);
			std::memcpy(hmap[ownHash].keyLong + len, 0, MAX_KEY_LENGTH - len);
		}
	}
}

inline uint64_t __attribute__((always_inline)) getMask64(__m128i a, __m128i b)
{
	return static_cast<uint64_t>(_mm_movemask_epi8(a) | (static_cast<uint64_t>(_mm_movemask_epi8(b)) << 32ULL));
}

inline uint64_t __attribute__((always_inline)) getMask64(__m256i a, __m256i b)
{
	return static_cast<uint64_t>(_mm256_movemask_epi8(a) | (static_cast<uint64_t>(_mm256_movemask_epi8(b)) << 32ULL));
}

inline void __attribute__((always_inline)) handleLinePacked(const uint8_t *start, const uint8_t *end, HashBin *hmap, size_t &dataIndex)
{
	const uint8_t *data = start;
	size_t offset = 0;
	int lineStart = 0;
	while(data < end) [[likely]]
	{
		__m256i bytes32_0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(start + offset));
		__m256i bytes32_1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(start + offset + 32));
		__m256i semicolons32 = _mm256_set1_epi8(';');
		__m256i comparedSemicolons = _mm256_cmpeq_epi8(bytes32_0, semicolons32);
		__m256i comparedSemicolons2 = _mm256_cmpeq_epi8(bytes32_1, semicolons32);
		uint64_t semicolonsMask = getMask64(comparedSemicolons, comparedSemicolons2);
		__m128i index = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
		int pos = (start + offset + _tzcnt_u64(semicolonsMask)) - data; // count the number of trailing zero bits

		while(semicolonsMask) [[likely]]
		{
			const int len = pos;

			__m128i characters = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
			__m128i mask = _mm_cmplt_epi8(index, _mm_set1_epi8(pos));
			__m128i keyCharacters = _mm_and_si128(characters, mask);
			__m128i sumCharacters = _mm_add_epi8(keyCharacters, _mm_unpackhi_epi8(keyCharacters, keyCharacters));

			uint32_t ownHash = (static_cast<uint64_t>(_mm_cvtsi128_si64(sumCharacters)) * SMALL) >> SHL_CONST;

			pos += (data[pos+1] == '-'); // make sure that data[pos] is directly prior to first digit
			int sign = (data[pos] == '-') ? -1 : 1;
			ownHash %= BIN_COUNT;

			// code taken from curiouscoding.nl; use uint32_t to avoid UB
			uint32_t uuvalue;
			std::memcpy(&uuvalue, data + pos + 1, 4); 
			uuvalue <<= 8 * (data[pos + 2] == '.');
			constexpr uint64_t C = 1 + (10 << 16) + (100 << 24);
			uuvalue &= 0x0f000f0f;
			uuvalue = ((uuvalue * C) >> 24) & ((1 << 10) - 1);
			int value = static_cast<int>(uuvalue) * sign;

			semicolonsMask &= semicolonsMask - 1;
			auto currentData = data;
			data += pos + 5 + (data[pos + 3] == '.');
			pos = (start + offset + _tzcnt_u64(semicolonsMask)) - data;
			
			if(len <= 16) [[likely]]
			{
				// same as with handleLineSlow
				__m128i characters = _mm_loadu_si128(reinterpret_cast<const __m128i*>(currentData));
				__m128i index = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
				__m128i mask = _mm_cmplt_epi8(index, _mm_set1_epi8(len));
				__m128i keyCharacters = _mm_and_si128(characters, mask);

				__m128i binCharacters = _mm_load_si128(reinterpret_cast<const __m128i*>(hmap[ownHash].keyShort));
				__m128i neq = _mm_xor_si128(binCharacters, keyCharacters);

				if(_mm_test_all_zeros(neq, neq) || hmap[ownHash].length == 0) [[likely]]
				{
					// do nothing, testing first bin is generally faster
				} else
				{
					ownHash = (ownHash + 1) % BIN_COUNT;
					while(hmap[ownHash].length > 0)
					{
						__m128i binCharacters = _mm_load_si128(reinterpret_cast<const __m128i*>(hmap[ownHash].keyShort));
						if(mm128i_equal(keyCharacters, binCharacters)) [[likely]]
						{
							break;
						}
						ownHash = (ownHash + 1) % BIN_COUNT;
					}
				}
			} else if(len <= 32 && hmap[ownHash].length != 0) [[likely]]
			{
				__m256i characters = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(currentData));
				__m256i mask = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(strcmpMask32 + 32 - len));
				__m256i keyCharacters = _mm256_and_si256(characters, mask);

				__m256i binCharacters = _mm256_load_si256(reinterpret_cast<const __m256i*>(hmap[ownHash].keyShort));
				if(mm256i_equal(keyCharacters, binCharacters)) [[likely]]
				{
					// do nothing
				} else
				{
					ownHash = (ownHash + 1) % BIN_COUNT;
					while(hmap[ownHash].length > 0)
					{
						__m256i binCharacters = _mm256_load_si256(reinterpret_cast<const __m256i*>(hmap[ownHash].keyShort));
						if(mm256i_equal(keyCharacters, binCharacters)) [[likely]]
						{
							break;
						}
						ownHash = (ownHash + 1) % BIN_COUNT;
					}
				}
			} else
			{
				while(hmap[ownHash].length > 0)
				{
					if(hmap[ownHash].length == len) [[likely]]
					{
						int index = 0;
						while((index + 32) < len)
						{
							__m256i characters = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(currentData + index));
							__m256i binCharacters = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(hmap[ownHash].keyLong + index));
							if(!mm256i_equal(characters, binCharacters)) [[unlikely]]
							{
								goto NEXT_LOOP;
							}
							index += 32;
						}

						if(index <= 64) [[likely]]
						{
							__m256i mask = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(strcmpMask32 + 32 - (len - index)));
							__m256i characters = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(currentData + index));
							__m256i keyCharacters = _mm256_and_si256(characters, mask);
							__m256i binCharacters = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(hmap[ownHash].keyLong + index));
							
							if(mm256i_equal(keyCharacters, binCharacters)) [[likely]]
							{
								break;
							}
						} else
						{
							bool equal = true;
							for(int i = index; i < len; ++i)
							{
								if(currentData[i] != hmap[ownHash].keyLong[i])
								{
									equal = false;
									break;
								}
								if(equal) [[likely]]
								{
									break;
								}
							}
						}
					}
					NEXT_LOOP:
					ownHash = (ownHash + 1) % BIN_COUNT;
				}
			}

			hmap[ownHash].count++;
			hmap[ownHash].sum += value;
			if(hmap[ownHash].max < value) [[unlikely]]
			{
				hmap[ownHash].max = value;
			}
			if(hmap[ownHash].min > value) [[unlikely]]
			{
				hmap[ownHash].min = value;
			}

			if(hmap[ownHash].length == 0) [[unlikely]]
			{
				hmap[ownHash].length = len;
				hmap[ownHash].sum = value;
				hmap[ownHash].count = 1;
				hmap[ownHash].max = value;
				hmap[ownHash].min = value;
				if(len < 32)
				{
					std::memcpy(hmap[ownHash].keyShort, currentData, len);
					std::memcpy(hmap[ownHash].keyShort + len, 0, 32 - len);
				} else
				{
					hmap[ownHash].keyLong = new uint8_t[MAX_KEY_LENGTH];
					std::memcpy(hmap[ownHash].keyLong, currentData, len);
					std::memcpy(hmap[ownHash].keyLong + len, 0, MAX_KEY_LENGTH - len);
				}
			}
		}
		offset += 64;
	}
	dataIndex += static_cast<uint64_t>(data - start);
}

void findNextLineStart(const uint8_t* data, size_t N, size_t &index)
{
	if(index == 0)
	{
		return;
	}
	while(index < N && data[index - 1]!= '\n')
	{
		++index;
	}
}

size_t handleLineRaw(int tid, const uint8_t *data, size_t fromByte, size_t toByte, size_t fileSize, bool initialized)
{
	if(!initialized)
	{
		hMaps[tid] = globalHMaps + tid * BIN_COUNT;
		for(int i = 0; i < BIN_COUNT; ++i)
		{
			hMaps[tid][i].length = 0;
		}
	}

	size_t startIndex = fromByte;
	findNextLineStart(data, toByte, startIndex);

	constexpr size_t ILP_LEVEL = 2; // instruction level parallelism
	size_t BYTES_PER_THREAD = (toByte - startIndex) / ILP_LEVEL;
	size_t index0 = startIndex;
	size_t index1 = startIndex + BYTES_PER_THREAD;

	findNextLineStart(data, toByte, index1);

	size_t endIndex0 = index1 - 1;
	size_t endIndex1 = toByte;
	size_t endIndex0Pre = endIndex0 - 2 * MAX_KEY_LENGTH;
	size_t endIndex1Pre = endIndex1 - 2 * MAX_KEY_LENGTH;

	handleLinePacked(data + index0, data + endIndex0Pre, hMaps[tid], index0);
	handleLinePacked(data + index1, data + endIndex1Pre, hMaps[tid], index1);

	while(index0 < endIndex0)
	{
		handleLineSlow(data + index0, hMaps[tid], index0);
	}

	while(index1 < endIndex1)
	{
		handleLineSlow(data + index1, hMaps[tid], index1);
	}
	
	return index1; // return the beginning of the first line of the next block
}

void parallelAggregate(int tid, int nThreads, int nAggregate)
{
	for(int index = tid; index < nThreads; ++index)
	{
		for(int h = 0; h < BIN_COUNT; ++h)
		{
			if(hMaps[index][h].length > 0)
			{
				auto& bin = hMaps[index][h];
				std::string key;
				if(bin.length <= 32)
				{
					key = std::string(bin.keyShort, bin.keyShort + bin.length);// TODO: Consider string view?
				} else
				{
					key = std::string(bin.keyLong, bin.keyLong + bin.length);
				}
				auto& stats = partialStats[tid][key];
				stats.count += bin.count;
				stats.sum += bin.sum;
				stats.max = std::max(stats.max, bin.max);
				stats.min = std::min(stats.min, bin.min);
			}
		}
	}
}

void parallelAggregateLv2(int tid, int nAggregate, int nAggregateLv2)
{
	for(int index = nAggregateLv2 + tid; index < nAggregate; index += nAggregateLv2)
	{
		for(auto& [key, value] : partialStats[index])
		{
			auto& stats = partialStats[tid][key];
			stats.count += value.count;
			stats.sum += value.sum;
			stats.max = std::max(stats.max, value.max);
			stats.min = std::min(stats.min, value.min);
		}
	}
}

float roundTo1Decimal(float number)
{
	return std::round(number * 10.0f) / 10.0f;
}

int doEverything(int argc, char *argv[])
{
	if constexpr(DEBUG)
	{
		std::cout << "Using " << MAX_N_THREADS << " threads\n";
	}
	if constexpr(DEBUG)
	{
		std::cout << "PC has " << N_CORES << " physical cores\n";
	}

	MyTimer timer, timer1;
	timer.startCounter();

	timer1.startCounter();
	globalHMaps = reinterpret_cast<HashBin*>(memalign(sizeof(HashBin), static_cast<size_t>(MAX_N_THREADS * BIN_COUNT * sizeof(HashBin))));
	if constexpr(DEBUG)
	{
		std::cout << "Malloc cost = " << timer1.getCounterMsPrecise() << "\n";
	}

	timer1.startCounter();
	std::string filePath = "measurements.txt";
	if(argc > 1)
	{
		filePath = std::string(argv[1]);
	}

	int fd = open(filePath.c_str(), O_RDONLY);
	struct stat fileStat;
	fstat(fd, &fileStat);
	size_t fileSize = fileStat.st_size;

	void* mappedDataVoid = mmap(nullptr, fileSize + 128, PROT_READ, MAP_SHARED, fd,  0);
	
	const uint8_t *data = reinterpret_cast<uint8_t*>(mappedDataVoid);
	if constexpr (DEBUG)
	{
		std::cout << "init mmap file cost = " << timer1.getCounterMsPrecise() << "ms\n";
	}

	timer1.startCounter();
	size_t index = 0;

	bool tid0Inited = false;
	int nThreads = MAX_N_THREADS;
	if (fileSize >= 100'000'000 && MAX_N_THREADS > N_CORES)
	{
		// In the event of too many hash collisions, hyperthreading slows down the program due to L3 Cache issues
		// As a result it is better to use the first 1MB to gather statistics about the file
		// If the number of unique keys >= X then use hyperthreading (MAX_N_THREADS)
		// Else use only physical cores (N_CORES)
		// It works with all inputs regardless, but it allows it to work faster for more difficult inputs

		tid0Inited = true;
		size_t toByte = 50'000;
		index = handleLineRaw(0, data, 0, toByte, fileSize, false);

		int uniqueKeyCount = 0;
		for(int h = 0; h < BIN_COUNT; ++h)
		{
			if(hMaps[0][h].length > 0)
			{
				uniqueKeyCount++;
			}
		}
		if(uniqueKeyCount > 500)
		{
			nThreads = N_CORES;
		}
	}

	if constexpr(DEBUG)
	{
		std::cout << "number of threads: " << nThreads << "\n";
	}
	if constexpr(DEBUG)
	{
		std::cout << "Gather key stats cost: " << timer1.getCounterMsPrecise() << "ms\n";
	}

	timer1.startCounter();
	size_t remainingBytes = fileSize - index;
	if((remainingBytes / nThreads) < (4 * MAX_KEY_LENGTH))
	{
		nThreads = 1;
	}
	size_t bytesPerThread = remainingBytes / nThreads + 1;
	std::vector<size_t> tStart, tEnd;
	std::vector<std::thread> threads;
	for(int64_t tid = nThreads - 1; tid >= 0; tid--)
	{
		size_t starter = index + tid * bytesPerThread;
		size_t ender = index + (tid + 1) * bytesPerThread;
		if(ender > fileSize)
		{
			ender = fileSize;
		}
		if(tid)
		{
			threads.emplace_back([tid, data, starter, ender, fileSize](){
				handleLineRaw(tid, data, starter, ender, fileSize, false);
			});
		} else
		{
			handleLineRaw(0, data, starter, ender, fileSize, tid0Inited);
		}
	}

	for(auto& thread : threads)
	{
		thread.join();
	}

	if constexpr(DEBUG)
	{
		std::cout << "Parallel process file cost: " << timer1.getCounterMsPrecise() << "ms\n";
	}

	timer1.startCounter();
	const int nAggregate = ((nThreads >= 16) && (nThreads % 4) == 0) ? (nThreads >> 2) : 1;
	const int nAggregateLv2 = ((nAggregate >= 32) && (nAggregate % 4) == 0) ? (nAggregate >> 2) : 1;

	if(nAggregate > 1)
	{
		threads.clear();
		for(int tid = 1; tid < nAggregate; ++tid)
		{
			threads.emplace_back([tid, nThreads, nAggregate](){
				parallelAggregate(tid, nThreads, nAggregate);
			});
		}
		parallelAggregate(0, nThreads, nAggregate);
		for(auto& thread : threads)
		{
			thread.join();
		}

		threads.clear();
		for(int tid = 1; tid < nAggregateLv2; ++tid)
		{
			threads.emplace_back([tid, nAggregate, nAggregateLv2](){
				parallelAggregateLv2(tid, nAggregate, nAggregateLv2);
			});
		}
		parallelAggregateLv2(0, nAggregate, nAggregateLv2);

		for(auto& thread : threads)
		{
			thread.join();
		}

		// stats are now aggregated into partialStats[0 to nAggregateLv2]
		for(int tid = 0; tid < nAggregateLv2; ++tid)
		{
			for(auto& [key, value] : partialStats[tid])
			{
					auto& stats = finalStats[key];
					stats.count = value.count;
					stats.sum += value.sum;
					stats.max = std::max(stats.max, value.max);
					stats.min = std::min(stats.min, value.min);
			}
		}
	} else
	{
		for(int tid = 0; tid < nThreads; ++tid)
		{
			for(int h = 0; h < BIN_COUNT; ++h)
			{
				if(hMaps[tid][h].length > 0)
				{
					auto& bin = hMaps[tid][h];
					std::string key;
					if(bin.length <= 32)
					{
						key = std::string(bin.keyShort, bin.keyShort + bin.length);
					} else
					{
						key = std::string(bin.keyLong, bin.keyLong + bin.length);
					}
					auto& stats = finalStats[key];
					stats.count += bin.count;
					stats.sum += bin.sum;
					stats.max = std::max(stats.max, bin.max);
					stats.min =  std::min(stats.min, bin.min);
				}
			}
		}
	}
	if constexpr (DEBUG)
	{
		std::cout << "Aggregate stats cost: " << timer1.getCounterMsPrecise() << "ms\n";
	}

	timer1.startCounter();
	std::vector<std::pair<std::string, Stats>> results;
	for(auto& [key, value] : finalStats)
	{
		results.emplace_back(key, value);
	}

	std::sort(results.begin(), results.end(), [](const auto& l, const auto& r){
		return l.first < r.first;
	});

	std::ofstream outputFile("result.txt");
	outputFile << std::fixed << std::setprecision(1);
	outputFile << "{";
	for(size_t i = 0; i < results.size(); ++i)
	{
		const auto& result = results[i];
		const auto& stationName = result.first;
		const auto& stats = result.second;
		float average = roundTo1Decimal(static_cast<double>(stats.sum) / 10.0 / stats.sum);
		float myMax = roundTo1Decimal(static_cast<float>(stats.max) / 10.0);
		float myMin = roundTo1Decimal(static_cast<float>(stats.min) / 10.0);

		outputFile << stationName << "=" << myMin << "/" << average << "/" << myMax;
		if( i < (results.size() - 1))
		{
			outputFile << ", ";
		}
	}
	outputFile << "}\n";
	outputFile.close();
	if constexpr (DEBUG)
	{
		std::cout << "Output of stats cost: " << timer1.getCounterMsPrecise() << "ms\n";
	}
	
	if constexpr (DEBUG)
	{
		std::cout << "Total runtime inside main: " << timer.getCounterMsPrecise() << "ms\n";
	}

	kill(getppid(), SIGUSR1);

	return 0;
}

void handleSignal(int signum)
{
	if(signum == SIGUSR1)
	{
		exit(0);
	}
}

int main(int argc, char* argv[])
{
	std::signal(SIGUSR1, handleSignal);
	
	int pid = fork();
	int status;
	if(pid == 0)
	{
		doEverything(argc, argv);
	} else
	{
		waitpid(pid, &status, 0);
	}

	return 0;
}