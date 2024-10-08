#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#undef NDEBUG
#include <assert.h>

#include <iostream>
#include <vector>
#include <algorithm>
#include <queue>
#include <string>
#include <cstdint>
#include <cmath>
#include <sstream>

#include <chrono>
#include <ctime>
#include <sys/mman.h>
#include <fcntl.h>
#include <omp.h>
#include <thread>
#include <mutex>
#include <termios.h>

#include <parallel_hashmap/phmap.h>
#include <parallel_hashmap/btree.h>
// #include "robin_hood.h"

#include "engine.h"
#include "vbyte_encoding.h"
// #include "serialize.h"
#include "bloom.h"


void flush_token_stream(TokenStream* token_stream) {
	fwrite(
			&token_stream->num_terms,
			sizeof(uint32_t), 
			1, 
			token_stream->file
			);
	if (token_stream->num_terms == 0) return;

	fwrite(
			token_stream->term_ids, 
			sizeof(uint32_t), 
			token_stream->num_terms, 
			token_stream->file
			);
	fwrite(
			token_stream->term_freqs, 
			sizeof(uint8_t), 
			token_stream->num_terms, 
			token_stream->file
			);
	token_stream->num_terms = 0;
}

void init_token_stream(TokenStream* token_stream, const std::string& out_file) {
	token_stream->term_ids   = (uint32_t*)malloc(TOKEN_STREAM_CAPACITY * sizeof(uint32_t));
	token_stream->term_freqs = (uint8_t*)malloc(TOKEN_STREAM_CAPACITY * sizeof(uint8_t));
	token_stream->num_terms  = 0;
	token_stream->file = fopen(out_file.c_str(), "w+b");
}

void add_token(
		TokenStream* token_stream,
		uint32_t term_id,
		uint8_t term_freq,
		bool new_doc
		) {
	token_stream->term_ids[token_stream->num_terms]   = term_id | ((uint32_t)new_doc << 31);
	token_stream->term_freqs[token_stream->num_terms] = term_freq;
	++(token_stream->num_terms);

	if (token_stream->num_terms == TOKEN_STREAM_CAPACITY) {
		flush_token_stream(token_stream);
	}
}

void free_token_stream(TokenStream* token_stream) {
	free(token_stream->term_ids);
	free(token_stream->term_freqs);
}


void init_inverted_index_new(InvertedIndexNew* II) {
	II->doc_ids      = NULL;
	II->term_offsets = NULL;
	II->doc_freqs    = NULL;

	II->num_terms    = 0;
	II->num_docs     = 0;
	II->avg_doc_size = 0.0f;
}

void free_inverted_index_new(InvertedIndexNew* II) {
	free(II->doc_ids);
	free(II->term_offsets);
	free(II->doc_freqs);
}

void read_token_stream(
		InvertedIndexNew* II, 
		TokenStream* token_stream
		) {
	// Reset file pointer to beginning
	if (fseek(token_stream->file, 0, SEEK_SET) != 0) {
		printf("Error seeking file.");
		exit(1);
	}

	// Assume num_terms, num_docs, and avg_doc_size are known and set.
	assert(II->num_terms > 0);
	assert(II->num_docs > 0);
	assert(II->avg_doc_size > 0.0f);

	assert(II->term_offsets == NULL);
	II->term_offsets = (uint32_t*)malloc(II->num_terms * sizeof(uint32_t));

	// Calculate doc offsets from doc_freqs
	uint32_t offset = 0;
	II->term_offsets[0] = offset;
	for (size_t term_idx = 1; term_idx < II->num_terms; ++term_idx) {
		offset += II->doc_freqs[term_idx];
		II->term_offsets[term_idx] = offset;

		assert(II->doc_freqs[term_idx] <= II->num_docs);
	}
	offset += II->doc_freqs[0];

	II->doc_ids = (tf_df_t*)malloc(offset * sizeof(tf_df_t));

	uint32_t* num_docs_read = (uint32_t*)malloc(II->num_terms * sizeof(uint32_t));
	memset(num_docs_read, 0, II->num_terms * sizeof(uint32_t));

	// Read token_stream->file in chunks
	uint32_t num_tokens = TOKEN_STREAM_CAPACITY;
	uint32_t doc_id = 0;
	while (num_tokens == TOKEN_STREAM_CAPACITY) {
		fread(
				&num_tokens,
				sizeof(uint32_t),
				1, 
				token_stream->file
				);
		assert(num_tokens <= TOKEN_STREAM_CAPACITY);

		if (num_tokens == 0) break;

		fread(
			token_stream->term_ids,
			sizeof(uint32_t),
			num_tokens,
			token_stream->file
			);
		fread(
			token_stream->term_freqs,
			sizeof(uint8_t),
			num_tokens,
			token_stream->file
			);

		for (size_t idx = 0; idx < num_tokens; ++idx) {
			if (token_stream->term_ids[idx] == UINT32_MAX) {
				++doc_id;
				continue;
			}

			// Pop highest bit to determine if new doc
			doc_id += (token_stream->term_ids[idx] >> 31);

			size_t term_id = (size_t)(token_stream->term_ids[idx] & 0x7FFFFFFF);

			tf_df_t entry;
			entry.tf     = token_stream->term_freqs[idx];
			entry.doc_id = doc_id;

			if (doc_id >= II->num_docs) {
				printf("Doc ID: %u\n", doc_id);
				printf("Num Docs: %u\n", II->num_docs);
				printf("Term freq: %u\n", entry.tf);
				printf("Term ID: %lu\n", term_id);
				printf("Tokens remaining: %lu\n", num_tokens - idx);
				fflush(stdout);
			}

			// TODO: Recheck this. Probably should be <
			// assert(doc_id < II->num_docs);

			uint32_t II_idx = II->term_offsets[term_id] + num_docs_read[term_id]++;
			assert(II_idx < offset);

			II->doc_ids[II_idx] = entry;
		}
	}
	// assert(doc_id < II->num_docs);

	free(num_docs_read);
	free_token_stream(token_stream);
	fclose(token_stream->file);
}

/*
void read_token_stream(
		InvertedIndexNew* II, 
		TokenStream* token_stream
		) {
	// Reset file pointer to beginning
	if (fseek(token_stream->file, 0, SEEK_SET) != 0) {
		printf("Error seeking file.");
		exit(1);
	}

	// Assume num_terms, num_docs, and avg_doc_size are known and set.
	assert(II->num_terms > 0);
	assert(II->num_docs > 0);
	assert(II->avg_doc_size > 0.0f);

	assert(II->term_offsets == NULL);
	II->term_offsets = (uint32_t*)malloc(II->num_terms * sizeof(uint32_t));

	II->doc_ids = (roaring_bitmap_t**)malloc(II->num_terms * sizeof(roaring_bitmap_t*));

	// Calculate doc offsets from doc_freqs
	uint32_t offset = 0;
	II->term_offsets[0] = offset;
	II->doc_ids[0] = roaring_bitmap_create_with_capacity(II->doc_freqs[0]);
	for (size_t term_idx = 1; term_idx < II->num_terms; ++term_idx) {
		offset += II->doc_freqs[term_idx];
		II->term_offsets[term_idx] = offset;

		II->doc_ids[term_idx] = roaring_bitmap_create_with_capacity(II->doc_freqs[term_idx]);

		assert(II->doc_freqs[term_idx] <= II->num_docs);
	}
	offset += II->doc_freqs[0];

	// uint32_t* num_docs_read = (uint32_t*)malloc(II->num_terms * sizeof(uint32_t));
	// memset(num_docs_read, 0, II->num_terms * sizeof(uint32_t));

	// Read token_stream->file in chunks
	uint32_t num_tokens = TOKEN_STREAM_CAPACITY;
	uint32_t doc_id = 0;
	while (num_tokens == TOKEN_STREAM_CAPACITY) {
		fread(
				&num_tokens,
				sizeof(uint32_t),
				1, 
				token_stream->file
				);
		assert(num_tokens <= TOKEN_STREAM_CAPACITY);

		if (num_tokens == 0) break;

		fread(
			token_stream->term_ids,
			sizeof(uint32_t),
			num_tokens,
			token_stream->file
			);
		fread(
			token_stream->term_freqs,
			sizeof(uint8_t),
			num_tokens,
			token_stream->file
			);

		for (size_t idx = 0; idx < num_tokens; ++idx) {
			if (token_stream->term_ids[idx] == UINT32_MAX) {
				++doc_id;
				continue;
			}

			// Pop highest bit to determine if new doc
			doc_id += (token_stream->term_ids[idx] >> 31);

			size_t term_id = (size_t)(token_stream->term_ids[idx] & 0x7FFFFFFF);

			tf_df_t entry;
			entry.tf     = token_stream->term_freqs[idx];
			entry.doc_id = doc_id;

			if (doc_id >= II->num_docs) {
				printf("Doc ID: %u\n", doc_id);
				printf("Num Docs: %u\n", II->num_docs);
				printf("Term freq: %u\n", entry.tf);
				printf("Term ID: %lu\n", term_id);
				printf("Tokens remaining: %lu\n", num_tokens - idx);
				fflush(stdout);
			}

			// TODO: Recheck this. Probably should be <
			// assert(doc_id < II->num_docs);

			// uint32_t II_idx = II->term_offsets[term_id] + num_docs_read[term_id]++;
			// assert(II_idx < offset);

			// II->doc_ids[II_idx] = entry;
			roaring_bitmap_add(II->doc_ids[II->term_offsets[term_id]], doc_id);
		}
	}
	// assert(doc_id < II->num_docs);

	// free(num_docs_read);
	free_token_stream(token_stream);
	fclose(token_stream->file);
}
*/

uint64_t calc_inverted_index_size(const InvertedIndexNew* II) {
	uint64_t size = 0;

	// Num tokens
	uint64_t num_tokens = II->term_offsets[II->num_terms - 1] + II->doc_freqs[II->num_terms - 1];

	// doc_ids
	size += num_tokens * sizeof(tf_df_t);

	// doc_sizes
	size += II->num_docs * sizeof(uint16_t);

	// term_offsets + doc_freqs
	size += II->num_terms * 2 * sizeof(uint32_t);

	// num_terms + num_docs + avg_doc_size
	size += 2 * sizeof(uint32_t) + sizeof(float);

	return size;
}

void init_bm25_partition_new(BM25PartitionNew* IP, uint64_t num_docs, uint16_t num_cols) {
	assert(num_cols > 0);
	assert(num_cols < 4096);

	IP->II = (InvertedIndexNew*)malloc(num_cols * sizeof(InvertedIndexNew));
	IP->unique_term_mappings = new MAP<std::string, uint32_t>[num_cols];

	IP->line_offsets = (uint64_t*)malloc(num_docs * sizeof(uint64_t));
	IP->num_docs = num_docs;
}

void free_bm25_partition_new(BM25PartitionNew* IP) {
	free(IP->II);
	free(IP->unique_term_mappings);
	free(IP->line_offsets);
}

uint64_t _BM25::get_doc_freqs_sum(
		std::string& term, 
		uint16_t col_idx
		) {
	uint64_t doc_freqs_sum = 0;

	for (size_t i = 0; i < num_partitions; ++i) {
		BM25PartitionNew* IP = &index_partitions[i];

		auto it = IP->unique_term_mappings[col_idx].find(term);
		if (it != IP->unique_term_mappings[col_idx].end()) {
			doc_freqs_sum += IP->II[col_idx].doc_freqs[it->second];
		}
	}
	return doc_freqs_sum;
}

BloomEntry init_bloom_entry(
		double fpr, 
		MAP<uint8_t, uint64_t>& tf_map,
		uint64_t min_df_bloom 
		) {
	BloomEntry bloom_entry;

	assert(min_df_bloom > 0);
	for (const auto& [term_freq, num_docs] : tf_map) {
		BloomFilter bf = init_bloom_filter(num_docs, fpr);
		// ChunkedBloomFilter bf = init_chunked_bloom_filter(min_df_bloom, fpr);
		bloom_entry.bloom_filters.insert({term_freq, bf});
	}
	return bloom_entry;
}

static inline ssize_t rfc4180_getline(char** lineptr, size_t* n, FILE* stream) {
    if (lineptr == nullptr || n == nullptr || stream == nullptr) {
        return -1;
    }

    size_t len = 0;
    int c;
    int in_quotes = 0;

    if (*lineptr == nullptr) {
        *n = 128;
        *lineptr = (char *)malloc(*n);
        if (*lineptr == nullptr) {
            return -1;
        }
    }

    while ((c = fgetc(stream)) != EOF) {
        if (len + 1 >= *n) {
            *n *= 2;
            char *new_lineptr = (char *)realloc(*lineptr, *n);
            if (new_lineptr == nullptr) {
                return -1;
            }
            *lineptr = new_lineptr;
        }

        (*lineptr)[len++] = c;

        if (c == '"') {
            in_quotes = !in_quotes;
        } else if (c == '\n' && !in_quotes) {
            break;
        }
    }

    if (ferror(stream)) {
        return -1;
    }

    if (len == 0 && c == EOF) {
        return -1;
    }

    (*lineptr)[len] = '\0';
    return len;
}

static inline ssize_t json_getline(char** lineptr, size_t* n, FILE* stream) {
    if (lineptr == nullptr || n == nullptr || stream == nullptr) {
        return -1;
    }

    size_t len = 0;
    int c;
    int in_quotes = 0;

    if (*lineptr == nullptr) {
        *n = 128;
        *lineptr = (char *)malloc(*n);
        if (*lineptr == nullptr) {
            return -1;
        }
    }

    while ((c = fgetc(stream)) != EOF) {
        if (len + 1 >= *n) {
            *n *= 2;
            char *new_lineptr = (char *)realloc(*lineptr, *n);
            if (new_lineptr == nullptr) {
                return -1;
            }
            *lineptr = new_lineptr;
        }

        (*lineptr)[len++] = c;

        if (c == '"') {
            in_quotes = !in_quotes;
        } else if (c == '\n' && !in_quotes && (*lineptr)[len - 2] == '}') {
            break;
        }
    }

    if (ferror(stream)) {
        return -1;
    }

    if (len == 0 && c == EOF) {
        return -1;
    }

    (*lineptr)[len] = '\0';
    return len;
}


static inline bool is_valid_token(std::string& str) {
	return (str.size() > 1 || isalnum(str[0]));
}

bool output_is_terminal() {
	return isatty(fileno(stdout));
}

void set_raw_mode() {
    termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag &= ~(ICANON | ECHO); // Disable echo and canonical mode
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}

// Function to reset the terminal to normal mode
void reset_terminal_mode() {
    termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}

// Function to query the cursor position
void get_cursor_position(int& rows, int& cols) {
    set_raw_mode();

    // Send the ANSI code to report cursor position
    std::cout << "\x1b[6n" << std::flush;

    // Expecting response in the format: ESC[row;colR
    char ch;
    int rows_temp = 0, cols_temp = 0;
    int read_state = 0;

    while (std::cin.get(ch)) {
        if (ch == '\x1b') {
            read_state = 1;
        } else if (ch == '[' && read_state == 1) {
            read_state = 2;
        } else if (ch == 'R') {
            break;
        } else if (read_state == 2 && ch != ';') {
            rows_temp = rows_temp * 10 + (ch - '0');
        } else if (ch == ';') {
            read_state = 3;
        } else if (read_state == 3) {
            cols_temp = cols_temp * 10 + (ch - '0');
        }
    }

    reset_terminal_mode();

    rows = rows_temp;
    cols = cols_temp;
}

void get_terminal_size(int& rows, int& cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        perror("ioctl");
        exit(EXIT_FAILURE);
    }
    rows = ws.ws_row;
    cols = ws.ws_col;
}


void _BM25::init_terminal() {
	bool is_terminal = isatty(fileno(stdout));

	int col;
	if (is_terminal) {
		get_cursor_position(init_cursor_row, col);
		get_terminal_size(terminal_height, col);

		if (terminal_height - init_cursor_row < num_partitions + 1) {
			// Scroll and reposition cursor
			std::cout << "\x1b[" << num_partitions + 1 << "S";
			init_cursor_row -= num_partitions + 1;
		}
	}
}


void _BM25::determine_partition_boundaries_json() {
    FILE* f = reference_file_handles[0];

    struct stat sb;
    if (fstat(fileno(f), &sb) == -1) {
        std::cerr << "Error getting file size." << std::endl;
        std::exit(1);
    }

    size_t file_size = sb.st_size;

    size_t byte_offset = header_bytes;
    fseek(f, byte_offset, SEEK_SET);

    std::vector<uint64_t> line_offsets;
    line_offsets.reserve(file_size / 64);

	// Use mmap instead
	char* file_data = (char*)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fileno(f), 0);
	if (file_data == MAP_FAILED) {
		std::cerr << "Error mapping file to memory." << std::endl;
		std::exit(1);
	}

	uint64_t cntr = 0;
    size_t file_pos = header_bytes;
	while (file_pos < file_size - 1) {
		if (file_data[file_pos++] == '\\') {
			++file_pos;
			continue;
		}

		// Skip to next unescaped quote
		if (file_data[file_pos] == '"') {
			++file_pos;

			while (1) {

				// Escape quote. Continue to next character.
				if (file_data[file_pos] == '\\') {
					file_pos += 2;
					continue;
				} else if (file_data[file_pos] == '"') {
					++file_pos;
					break;
				} else {
					++file_pos;
				}
			}
		}

		if (file_data[file_pos] == '}' && file_data[file_pos + 1] == '\n') {
			file_pos += 2;
			line_offsets.push_back(file_pos);
			if (cntr++ % 1000 == 0) {
				printf("%luK lines read\r", cntr / 1000);
				fflush(stdout);
			}
		}
	}

    uint64_t num_lines  = line_offsets.size();
    size_t   chunk_size = num_lines / num_partitions;

	index_partitions = (BM25PartitionNew*)malloc(num_partitions * sizeof(BM25PartitionNew));
    for (size_t i = 0; i < num_partitions; ++i) {
        partition_boundaries.push_back(line_offsets[i * chunk_size]);

        BM25PartitionNew* IP = &index_partitions[i];
		init_bm25_partition_new(
				&index_partitions[i],
				chunk_size,
				search_cols.size()
				);

		size_t idx = 0;
		for (size_t j = i * chunk_size; j < (i + 1) * chunk_size; ++j) {
			IP->line_offsets[idx++] = line_offsets[j];
		}
    }
    partition_boundaries.push_back(line_offsets.back());

	assert((uint16_t)partition_boundaries.size() == num_partitions + 1);

    // Reset file pointer to beginning
    fseek(f, header_bytes, SEEK_SET);
}

void _BM25::proccess_csv_header() {
	// Iterate over first line to get column names.
	// If column name matches search_col, set search_column_index.

	FILE* f = reference_file_handles[0];
	char* line = NULL;
	size_t len = 0;
	// search_col_idx = -1;

	fseek(f, 0, SEEK_SET);

	// Get col names
	ssize_t read = getline(&line, &len, f);
	std::istringstream iss(line);
	std::string value;
	while (std::getline(iss, value, ',')) {
		if (value.find("\n") != std::string::npos) {
			value.erase(value.find("\n"));
		}
		std::transform(value.begin(), value.end(), value.begin(), ::tolower);
		columns.push_back(value);
		for (std::string col : search_cols) {
			std::transform(col.begin(), col.end(), col.begin(), ::tolower);
			if (value == col) {
				search_col_idxs.push_back(columns.size() - 1);
			}
		}
	}

	if (search_col_idxs.empty()) {
		std::cerr << "Search column not found in header" << std::endl;
		std::cerr << "Cols found:  ";
		for (size_t i = 0; i < columns.size(); ++i) {
			std::cerr << columns[i] << ",";
		}
		std::cerr << std::endl;
		std::cout << std::flush;
		exit(1);
	}

	std::sort(search_col_idxs.begin(), search_col_idxs.end());

	header_bytes = read;
	free(line);
}

void _BM25::determine_partition_boundaries_csv_rfc_4180() {
    FILE* f = reference_file_handles[0];

    struct stat sb;
    if (fstat(fileno(f), &sb) == -1) {
        std::cerr << "Error getting file size." << std::endl;
        std::exit(1);
    }

    size_t file_size = sb.st_size;

    size_t byte_offset = header_bytes;
    fseek(f, byte_offset, SEEK_SET);

    std::vector<uint64_t> line_offsets;
    line_offsets.reserve(file_size / 64);

	// Use mmap instead
	char* file_data = (char*)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fileno(f), 0);
	if (file_data == MAP_FAILED) {
		std::cerr << "Error mapping file to memory." << std::endl;
		std::exit(1);
	}

    size_t file_pos = header_bytes;
	line_offsets.push_back(header_bytes);
	while (file_pos < file_size - 1) {
		if (file_data[file_pos] == '"') {
			// Skip to next unescaped quote
			++file_pos;

			while (1) {

				// Escape quote. Continue to next character.
				if (file_data[file_pos] == '"' && file_data[file_pos + 1] == '"') {
					file_pos += 2;
					continue;
				} else if (file_data[file_pos] == '"') {
					++file_pos;
					break;
				} else {
					++file_pos;
				}
			}
		}
		if (file_data[file_pos++] == '\n') {
			line_offsets.push_back(file_pos);

			// if (cntr++ % 10000 == 0) {
				// printf("%luK lines read\r", cntr / 10000);
			// }
		}
	}

    uint64_t num_lines  = line_offsets.size();
    size_t   chunk_size = num_lines / num_partitions;
	size_t   final_chunk_size = chunk_size + (num_lines % num_partitions);

	// TODO: Fix last chunk size issue on json.
	index_partitions = (BM25PartitionNew*)malloc(num_partitions * sizeof(BM25PartitionNew));
    for (size_t i = 0; i < num_partitions; ++i) {
		partition_boundaries.push_back(line_offsets[i * chunk_size]);

		size_t current_chunk_size = (i != (size_t)num_partitions - 1) ? chunk_size : final_chunk_size;

		BM25PartitionNew* IP = &index_partitions[i];
		init_bm25_partition_new(
				&index_partitions[i],
				current_chunk_size,
				search_cols.size()
				);

		size_t idx = 0;
		for (size_t j = i * chunk_size; j < (i * chunk_size) + current_chunk_size; ++j) {
			IP->line_offsets[idx++] = line_offsets[j];
		}
    }
    partition_boundaries.push_back(line_offsets.back());

	assert((uint16_t)partition_boundaries.size() == num_partitions + 1);

    // Reset file pointer to beginning
    fseek(f, header_bytes, SEEK_SET);
}


inline RLEElement_u8 init_rle_element_u8(uint8_t value) {
	RLEElement_u8 rle;
	rle.num_repeats = 1;
	rle.value = value;
	return rle;
}

inline uint64_t get_rle_element_u8_size(const RLEElement_u8& rle_element) {
	return (uint64_t)rle_element.num_repeats;
}

bool check_rle_u8_row_size(const std::vector<RLEElement_u8>& rle_row, uint64_t max_size) {
	uint64_t size = 0;
	for (const auto& rle_element : rle_row) {
		size += get_rle_element_u8_size(rle_element);
		if (size >= max_size) {
			return true;
		}
	}
	return false;
}

inline uint64_t get_rle_u8_row_size(const std::vector<RLEElement_u8>& rle_row) {
	uint64_t size = 0;
	for (const auto& rle_element : rle_row) {
		size += get_rle_element_u8_size(rle_element);
	}
	return size;
}

void add_rle_element_u8(std::vector<RLEElement_u8>& rle_row, uint8_t value) {
	if (rle_row.empty()) {
		rle_row.push_back(init_rle_element_u8(value));
	}
	else {
		if (rle_row.back().value == value) {
			if (rle_row.back().num_repeats == 255) {
				rle_row.push_back(init_rle_element_u8(value));
			}
			else {
				++(rle_row.back().num_repeats);
			}
		}
		else {
			RLEElement_u8 rle = init_rle_element_u8(value);
			rle_row.push_back(rle);
		}
	}
}

/*
uint32_t _BM25::process_doc_partition(
		const char* doc,
		const char terminator,
		uint64_t doc_id,
		uint32_t& unique_terms_found,
		uint16_t partition_id,
		uint16_t col_idx
		) {
	BM25Partition& IP = index_partitions[partition_id];
	InvertedIndex& II = IP.II[col_idx];

	uint32_t char_idx = 0;

	std::string term = "";

	// robin_hood::unordered_flat_map<uint64_t, uint8_t> terms_seen;
	MAP<uint64_t, uint8_t> terms_seen;

	// Split by commas not inside double quotes
	uint64_t doc_size = 0;
	while (true) {
		if (char_idx > 1048576) {
			std::cout << "Search field not found on line: " << doc_id << std::endl;
			std::cout << "Doc: " << doc << std::endl;
			std::cout << std::flush;
			std::exit(1);
		}
		if (doc[char_idx] == '\\') {
			++char_idx;
			term += toupper(doc[char_idx]);
			++char_idx;
			continue;
		}

		if (terminator == ',' && doc[char_idx] == ',') {
			++char_idx;
			break;
		}

		if (doc[char_idx] == '\n') {
			++char_idx;
			break;
		}

		if (terminator == '"' && doc[char_idx] == '"') {
			if (doc[++char_idx] == ',') {
				break;
			}

			if (doc[char_idx] == terminator) {
				term += terminator;
				++char_idx;
				continue;
			}
		}

		if (doc[char_idx] == ' ' && term == "") {
			++char_idx;
			continue;
		}

		if (doc[char_idx] == ' ') {
			if ((stop_words.find(term) != stop_words.end()) || !is_valid_token(term)) {
				term.clear();
				++char_idx;
				++doc_size;
				continue;
			}

			auto [it, add] = IP.unique_term_mapping[col_idx].try_emplace(
					term, 
					unique_terms_found
					);
			if (add) {
				// New term
				terms_seen.insert({it->second, 1});
				II.inverted_index_compressed.emplace_back();
				II.prev_doc_ids.push_back(0);
				II.doc_freqs.push_back(1);
				++unique_terms_found;
			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
					++(II.doc_freqs[it->second]);
				}
				else {
					++(terms_seen[it->second]);
				}
			}

			++doc_size;
			term.clear();

			++char_idx;
			continue;
		}

		term += toupper(doc[char_idx]);
		++char_idx;
	}

	if (term != "") {
		if ((stop_words.find(term) == stop_words.end()) && is_valid_token(term)) {
			auto [it, add] = IP.unique_term_mapping[col_idx].try_emplace(
					term, 
					unique_terms_found
					);

			if (add) {
				// New term
				terms_seen.insert({it->second, 1});
				II.inverted_index_compressed.emplace_back();
				II.prev_doc_ids.push_back(0);
				II.doc_freqs.push_back(1);
				++unique_terms_found;
			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
					++(II.doc_freqs[it->second]);
				}
				else {
					++(terms_seen[it->second]);
				}
			}
		}
		++doc_size;
	}

	if (doc_id == IP.II[col_idx].doc_sizes.size() - 1) {
		IP.II[col_idx].doc_sizes[doc_id] += (uint16_t)doc_size;
	}
	else {
		IP.II[col_idx].doc_sizes.push_back((uint16_t)doc_size);
	}

	for (const auto& [term_idx, tf] : terms_seen) {
		compress_uint64_differential_single(
				II.inverted_index_compressed[term_idx].doc_ids,
				doc_id,
				II.prev_doc_ids[term_idx]
				);
		add_rle_element_u8(
				II.inverted_index_compressed[term_idx].term_freqs, 
				tf
				);
		II.prev_doc_ids[term_idx] = doc_id;
	}

	return char_idx;
}
*/

uint32_t _BM25::process_doc_partition_json(
		const char* doc,
		const char terminator,
		TokenStream* token_stream,
		uint64_t doc_id,
		uint16_t partition_id,
		uint16_t col_idx,
		uint32_t* doc_freqs_capacity
		) {
	BM25PartitionNew* IP = &index_partitions[partition_id];
	InvertedIndexNew* II = &IP->II[col_idx];

	uint32_t char_idx = 0;

	std::string term = "";

	MAP<uint64_t, uint8_t> terms_seen;

	// Split by commas not inside double quotes
	uint64_t doc_size = 0;
	while (true) {
		if (char_idx > 1048576) {
			printf("Search field not found on line: %lu", doc_id);
			printf("Doc: %s\n", doc);
			exit(1);
		}

		if (doc[char_idx] == '\\') {
			++char_idx;
			term += toupper(doc[char_idx]);
			++char_idx;
			continue;
		}

		if (terminator == ',' && doc[char_idx] == ',') {
			++char_idx;
			break;
		}

		if (terminator == '"' && doc[char_idx] == '"') {
			++char_idx;
			if (doc[char_idx] == ',' || doc[char_idx] == '}') {
				break;
			}
		}

		if (doc[char_idx] == ' ' && term == "") {
			++char_idx;
			continue;
		}

		if (doc[char_idx] == ' ') {
			if ((stop_words.find(term) != stop_words.end()) || !is_valid_token(term)) {
				term.clear();
				++char_idx;
				++doc_size;
				continue;
			}

			auto [it, add] = IP->unique_term_mappings[col_idx].try_emplace(
					term, 
					II->num_terms
					);
			if (add) {
				// New term
				terms_seen.insert({it->second, 1});

				if (II->num_terms + 1 == *doc_freqs_capacity) {
					*doc_freqs_capacity *= 2;
					II->doc_freqs = (uint32_t*)realloc(
							II->doc_freqs, 
							*doc_freqs_capacity * sizeof(uint32_t)
							);
				}

				II->doc_freqs[II->num_terms++] = 1;
			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
					++(II->doc_freqs[it->second]);
				}
				else {
					++(terms_seen[it->second]);
				}
			}

			++doc_size;
			term.clear();

			++char_idx;
			continue;
		}

		term += toupper(doc[char_idx]);
		++char_idx;
	}

	if (term != "") {
		if ((stop_words.find(term) == stop_words.end()) && is_valid_token(term)) {
			auto [it, add] = IP->unique_term_mappings[col_idx].try_emplace(
					term, 
					II->num_terms
					);

			if (add) {
				// New term
				terms_seen.insert({it->second, 1});

				if (II->num_terms + 1 == *doc_freqs_capacity) {
					*doc_freqs_capacity *= 2;
					II->doc_freqs = (uint32_t*)realloc(
							II->doc_freqs, 
							*doc_freqs_capacity * sizeof(uint32_t)
							);
				}

				II->doc_freqs[II->num_terms++] = 1;
			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
					++(II->doc_freqs[it->second]);
				}
				else {
					++(terms_seen[it->second]);
				}
			}
		}
		++doc_size;
	}

	IP->II[col_idx].doc_sizes[doc_id] = (uint16_t)doc_size;

	// Start as true unless it is the first document.
	bool first = doc_id != 0;
	for (const auto& [term_idx, tf] : terms_seen) {
		add_token(
				token_stream,
				term_idx,
				tf,
				first
				);
		first = false;
	}

	return char_idx;
}

/*
uint32_t _BM25::process_doc_partition_rfc_4180(
		const char* doc,
		const char terminator,
		uint64_t doc_id,
		uint32_t& unique_terms_found,
		uint16_t partition_id,
		uint16_t col_idx
		) {
	BM25PartitionNew* IP = &index_partitions[partition_id];
	InvertedIndexNew* II = &IP->II[col_idx];

	uint32_t char_idx = 0;

	std::string term = "";

	// robin_hood::unordered_flat_map<uint64_t, uint8_t> terms_seen;
	MAP<uint64_t, uint8_t> terms_seen;

	// Split by commas not inside double quotes
	uint64_t doc_size = 0;
	while (true) {
		if (char_idx > 1048576) {
			std::cout << "Search field not found on line: " << doc_id << std::endl;
			std::cout << "Doc: " << doc << std::endl;
			std::cout << std::flush;
			std::exit(1);
		}

		if (terminator == ',' && doc[char_idx] == ',') {
			++char_idx;
			break;
		}

		if (doc[char_idx] == '\n') {
			++char_idx;
			break;
		}

		if (terminator == '"' && doc[char_idx] == '"') {
			++char_idx;
			if (doc[char_idx] == ',' || doc[char_idx] == '\n') {
				break;
			}

			if (doc[char_idx] == terminator) {
				++char_idx;
				continue;
			}
		}

		if (doc[char_idx] == ' ' && term == "") {
			++char_idx;
			continue;
		}

		if (doc[char_idx] == ' ') {
			if ((stop_words.find(term) != stop_words.end()) || !is_valid_token(term)) {
				term.clear();
				++char_idx;
				++doc_size;
				continue;
			}

			auto [it, add] = IP->unique_term_mappings[col_idx].try_emplace(
					term, 
					unique_terms_found
					);
			if (add) {
				// New term
				terms_seen.insert({it->second, 1});
				++(II->num_terms);
				if (II->num_terms == *doc_freqs_capacity) {
					*doc_freqs_capacity *= 2;
					II->doc_freqs = (uint32_t*)realloc(
							II->doc_freqs, 
							*doc_freqs_capacity * sizeof(uint32_t)
							);
				}

				II->doc_freqs[II->num_terms] = 1;
				++unique_terms_found;

			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
					++(II.doc_freqs[it->second]);
				}
				else {
					++(terms_seen[it->second]);
				}
			}

			++doc_size;
			term.clear();

			++char_idx;
			continue;
		}

		term += toupper(doc[char_idx]);
		++char_idx;
	}

	if (term != "") {
		if ((stop_words.find(term) == stop_words.end()) && is_valid_token(term)) {
			auto [it, add] = IP.unique_term_mapping[col_idx].try_emplace(
					term, 
					unique_terms_found
					);

			if (add) {
				// New term
				terms_seen.insert({it->second, 1});
				II.inverted_index_compressed.emplace_back();
				II.prev_doc_ids.push_back(0);
				II.doc_freqs.push_back(1);
				++unique_terms_found;
			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
					++(II.doc_freqs[it->second]);
				}
				else {
					++(terms_seen[it->second]);
				}
			}
		}
		++doc_size;
	}

	if (doc_id == IP.II[col_idx].doc_sizes.size() - 1) {
		IP.II[col_idx].doc_sizes[doc_id] += (uint16_t)doc_size;
	}
	else {
		IP.II[col_idx].doc_sizes.push_back((uint16_t)doc_size);
	}

	for (const auto& [term_idx, tf] : terms_seen) {
		compress_uint64_differential_single(
				II.inverted_index_compressed[term_idx].doc_ids,
				doc_id,
				II.prev_doc_ids[term_idx]
				);
		add_rle_element_u8(
				II.inverted_index_compressed[term_idx].term_freqs, 
				tf
				);
		II.prev_doc_ids[term_idx] = doc_id;
	}

	return char_idx;
}

uint32_t _BM25::_process_doc_partition_rfc_4180_quoted(
		const char* doc,
		uint64_t doc_id,
		uint32_t& unique_terms_found,
		uint16_t partition_id,
		uint16_t col_idx
		) {
	BM25Partition& IP = index_partitions[partition_id];
	InvertedIndex& II = IP.II[col_idx];

	uint32_t char_idx = 0;

	std::string term = "";

	// robin_hood::unordered_flat_map<uint64_t, uint8_t> terms_seen;
	MAP<uint64_t, uint8_t> terms_seen;

	// Split by commas not inside double quotes
	uint64_t doc_size = 0;
	while (true) {
		if (char_idx > 1048576) {
			std::cout << "Search field not found on line: " << doc_id << std::endl;
			std::cout << "Doc: " << doc << std::endl;
			std::cout << std::flush;
			std::exit(1);
		}

		if (doc[char_idx] == '"') {
			++char_idx;
			if (doc[char_idx] == ',' || doc[char_idx] == '\n') {
				break;
			}

			if (doc[char_idx] == '"') {
				++char_idx;
				continue;
			}
		}

		if ((doc[char_idx] == ' ' || doc[char_idx] == ' ') && term == "") {
			++char_idx;
			continue;
		}

		if ((doc[char_idx] == ' ' || doc[char_idx] == ' ')) {
			if ((stop_words.find(term) != stop_words.end()) || !is_valid_token(term)) {
				term.clear();
				++char_idx;
				++doc_size;
				continue;
			}

			auto [it, add] = IP.unique_term_mapping[col_idx].try_emplace(
					term, 
					unique_terms_found
					);
			if (add) {
				// New term
				terms_seen.insert({it->second, 1});
				II.inverted_index_compressed.emplace_back();
				II.prev_doc_ids.push_back(0);
				II.doc_freqs.push_back(1);
				++unique_terms_found;
			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
					++(II.doc_freqs[it->second]);
				}
				else {
					++(terms_seen[it->second]);
				}
			}

			++doc_size;
			term.clear();

			++char_idx;
			continue;
		}

		term += toupper(doc[char_idx]);
		++char_idx;
	}

	if (term != "") {
		if ((stop_words.find(term) == stop_words.end()) && is_valid_token(term)) {
			auto [it, add] = IP.unique_term_mapping[col_idx].try_emplace(
					term, 
					unique_terms_found
					);

			if (add) {
				// New term
				terms_seen.insert({it->second, 1});
				II.inverted_index_compressed.emplace_back();
				II.prev_doc_ids.push_back(0);
				II.doc_freqs.push_back(1);
				++unique_terms_found;
			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
					++(II.doc_freqs[it->second]);
				}
				else {
					++(terms_seen[it->second]);
				}
			}
		}
		++doc_size;
	}

	if (doc_id == IP.II[col_idx].doc_sizes.size() - 1) {
		IP.II[col_idx].doc_sizes[doc_id] += (uint16_t)doc_size;
	}
	else {
		IP.II[col_idx].doc_sizes.push_back((uint16_t)doc_size);
	}

	for (const auto& [term_idx, tf] : terms_seen) {
		compress_uint64_differential_single(
				II.inverted_index_compressed[term_idx].doc_ids,
				doc_id,
				II.prev_doc_ids[term_idx]
				);
		add_rle_element_u8(
				II.inverted_index_compressed[term_idx].term_freqs, 
				tf
				);
		II.prev_doc_ids[term_idx] = doc_id;
	}

	return char_idx;
}
*/

uint32_t _BM25::process_doc_partition_rfc_4180_v2(
		const char* doc,
		const char terminator,
		TokenStream* token_stream,
		uint64_t doc_id,
		size_t partition_id,
		size_t col_idx,
		uint32_t* doc_freqs_capacity
		) {
	BM25PartitionNew* IP = &index_partitions[partition_id];
	InvertedIndexNew* II = &IP->II[col_idx];

	uint32_t char_idx = 0;

	std::string term = "";

	MAP<uint64_t, uint8_t> terms_seen;

	// Split by commas not inside double quotes
	uint64_t doc_size = 0;
	while (true) {
		if (char_idx > 1048576) {
			printf("Search field not found on line: %lu\n", doc_id);
			printf("Doc: %s\n", doc);
			exit(1);
		}

		if (terminator == ',' && doc[char_idx] == ',') {
			++char_idx;
			break;
		}

		if (doc[char_idx] == '\n') {
			++char_idx;
			break;
		}

		if (terminator == '"' && doc[char_idx] == '"') {
			++char_idx;
			if (doc[char_idx] == ',' || doc[char_idx] == '\n') {
				break;
			}

			if (doc[char_idx] == terminator) {
				++char_idx;
				continue;
			}
		}

		if (doc[char_idx] == ' ' && term == "") {
			++char_idx;
			continue;
		}

		if (doc[char_idx] == ' ') {
			if ((stop_words.find(term) != stop_words.end()) || !is_valid_token(term)) {
				term.clear();
				++char_idx;
				++doc_size;
				continue;
			}

			auto [it, add] = IP->unique_term_mappings[col_idx].try_emplace(
					term, 
					II->num_terms
					);
			if (add) {
				// New term
				terms_seen.insert({it->second, 1});

				if (II->num_terms + 1 == *doc_freqs_capacity) {
					*doc_freqs_capacity *= 2;
					II->doc_freqs = (uint32_t*)realloc(
							II->doc_freqs, 
							*doc_freqs_capacity * sizeof(uint32_t)
							);
				}

				II->doc_freqs[II->num_terms++] = 1;

			} else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
					++(II->doc_freqs[it->second]);
				} else {
					++(terms_seen[it->second]);
				}
			}

			++doc_size;
			term.clear();

			++char_idx;
			continue;
		}

		term += toupper(doc[char_idx]);
		++char_idx;
	}

	if (term != "") {
		if ((stop_words.find(term) == stop_words.end()) && is_valid_token(term)) {
			auto [it, add] = IP->unique_term_mappings[col_idx].try_emplace(
					term, 
					II->num_terms
					);

			if (add) {
				// New term
				terms_seen.insert({it->second, 1});

				if (II->num_terms + 1 == *doc_freqs_capacity) {
					*doc_freqs_capacity *= 2;
					II->doc_freqs = (uint32_t*)realloc(
							II->doc_freqs, 
							*doc_freqs_capacity * sizeof(uint32_t)
							);
				}

				II->doc_freqs[II->num_terms++] = 1;
			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
					++(II->doc_freqs[it->second]);
				}
				else {
					++(terms_seen[it->second]);
				}
			}
		}
		++doc_size;
	}

	// When other col for doc has already been processed.
	IP->II[col_idx].doc_sizes[doc_id] = (uint16_t)doc_size;

	if (terms_seen.empty()) {
		add_token(
				token_stream,
				UINT32_MAX,
				UINT8_MAX,
				true
				);
		return char_idx;
	}

	// Start as true unless it is the first document.
	bool first = doc_id != 0;
	for (const auto& [term_idx, tf] : terms_seen) {
		add_token(
				token_stream,
				term_idx,
				tf,
				first
				);
		first = false;
	}

	return char_idx;
}

uint32_t _BM25::_process_doc_partition_rfc_4180_quoted_v2(
		const char* doc,
		TokenStream* token_stream,
		uint64_t doc_id,
		size_t partition_id,
		size_t col_idx,
		uint32_t* doc_freqs_capacity
		) {
	BM25PartitionNew* IP = &index_partitions[partition_id];
	InvertedIndexNew* II = &IP->II[col_idx];

	uint32_t char_idx = 0;

	std::string term = "";

	MAP<uint64_t, uint8_t> terms_seen;

	// Split by commas not inside double quotes
	uint64_t doc_size = 0;
	while (true) {
		if (char_idx > 1048576) {
			printf("Search field not found on line: %lu", doc_id);
			printf("Doc: %s\n", doc);
			exit(1);
		}

		if (doc[char_idx] == '"') {
			++char_idx;
			if (doc[char_idx] == ',' || doc[char_idx] == '\n') {
				break;
			}

			if (doc[char_idx] == '"') {
				++char_idx;
				continue;
			}
		}

		if ((doc[char_idx] == ' ' || doc[char_idx] == ' ') && term == "") {
			++char_idx;
			continue;
		}

		if ((doc[char_idx] == ' ' || doc[char_idx] == ' ')) {
			if ((stop_words.find(term) != stop_words.end()) || !is_valid_token(term)) {
				term.clear();
				++char_idx;
				++doc_size;
				continue;
			}

			auto [it, add] = IP->unique_term_mappings[col_idx].try_emplace(
					term, 
					II->num_terms
					);
			if (add) {
				// New term
				terms_seen.insert({it->second, 1});

				if (II->num_terms + 1 == *doc_freqs_capacity) {
					*doc_freqs_capacity *= 2;
					II->doc_freqs = (uint32_t*)realloc(
							II->doc_freqs, 
							*doc_freqs_capacity * sizeof(uint32_t)
							);
				}

				II->doc_freqs[II->num_terms++] = 1;
			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
					++(II->doc_freqs[it->second]);
				}
				else {
					++(terms_seen[it->second]);
				}
			}

			++doc_size;
			term.clear();

			++char_idx;
			continue;
		}

		term += toupper(doc[char_idx]);
		++char_idx;
	}

	if (term != "") {
		if ((stop_words.find(term) == stop_words.end()) && is_valid_token(term)) {
			auto [it, add] = IP->unique_term_mappings[col_idx].try_emplace(
					term, 
					II->num_terms
					);

			if (add) {
				// New term
				terms_seen.insert({it->second, 1});

				if (II->num_terms + 1 == *doc_freqs_capacity) {
					*doc_freqs_capacity *= 2;
					II->doc_freqs = (uint32_t*)realloc(
							II->doc_freqs, 
							*doc_freqs_capacity * sizeof(uint32_t)
							);
				}

				II->doc_freqs[II->num_terms++] = 1;
			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
					++(II->doc_freqs[it->second]);
				}
				else {
					++(terms_seen[it->second]);
				}
			}
		}
		++doc_size;
	}

	IP->II[col_idx].doc_sizes[doc_id] = (uint16_t)doc_size;

	if (terms_seen.empty()) {
		add_token(
				token_stream,
				UINT32_MAX,
				UINT8_MAX,
				true
				);
		return char_idx;
	}

	// Start as true unless it is the first document.
	bool first = doc_id != 0;
	for (const auto& [term_idx, tf] : terms_seen) {
		add_token(
				token_stream,
				term_idx,
				tf,
				first
				);
		first = false;
	}

	return char_idx;
}


/*
void _BM25::process_doc_partition_rfc_4180_mmap(
		const char* file_data,
		const char terminator,
		uint64_t doc_id,
		uint32_t& unique_terms_found,
		uint16_t partition_id,
		uint16_t col_idx,
		uint64_t& byte_offset
		) {
	BM25Partition& IP = index_partitions[partition_id];
	InvertedIndex& II = IP.II[col_idx];

	std::string term = "";

	// robin_hood::unordered_flat_map<uint64_t, uint8_t> terms_seen;
	MAP<uint64_t, uint8_t> terms_seen;

	// Split by commas not inside double quotes
	uint64_t doc_size = 0;
	while (true) {
		if (doc_size > 131072) {
			std::cout << "Search field not found on line: " << doc_id << std::endl;
			std::cout << std::flush;
			std::exit(1);
		}

		if (terminator == '"' && file_data[byte_offset] == '"') {
			++byte_offset;
			if (file_data[byte_offset] == ',' || file_data[byte_offset] == '\n') {
				++byte_offset;
				break;
			}

			if (file_data[byte_offset] == terminator) {
				// Escaped quote. Continue.
				++byte_offset;
				continue;
			}
		}

		if ((file_data[byte_offset] == terminator) && (terminator != '"')) {
			++byte_offset;
			break;
		}

		// Whitespace. Add term if not empty.
		if (file_data[byte_offset] == ' ') {
			if (term == "") {
				++byte_offset;
				continue;
			} else {
				if ((stop_words.find(term) != stop_words.end()) || !is_valid_token(term)) {
					term.clear();
					++byte_offset;
					++doc_size;
					continue;
				}

				auto [it, add] = IP.unique_term_mapping[col_idx].try_emplace(
						term, 
						unique_terms_found
						);
				if (add) {
					// New term
					terms_seen.insert({it->second, 1});
					II.inverted_index_compressed.emplace_back();
					II.prev_doc_ids.push_back(0);
					II.doc_freqs.push_back(1);
					++unique_terms_found;
				}
				else {
					// Term already exists
					if (terms_seen.find(it->second) == terms_seen.end()) {
						terms_seen.insert({it->second, 1});
						++(II.doc_freqs[it->second]);
					}
					else {
						++(terms_seen[it->second]);
					}
				}

				++doc_size;
				term.clear();

				++byte_offset;
				continue;
			}
		}

		term += toupper(file_data[byte_offset]);
		++byte_offset;
	}

	if (term != "") {
		if ((stop_words.find(term) == stop_words.end()) && is_valid_token(term)) {
			auto [it, add] = IP.unique_term_mapping[col_idx].try_emplace(
					term, 
					unique_terms_found
					);

			if (add) {
				// New term
				terms_seen.insert({it->second, 1});
				II.inverted_index_compressed.emplace_back();
				II.prev_doc_ids.push_back(0);
				II.doc_freqs.push_back(1);
				++unique_terms_found;
			}
			else {
				// Term already exists
				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
					++(II.doc_freqs[it->second]);
				}
				else {
					++(terms_seen[it->second]);
				}
			}
		}
		++doc_size;
	}

	if (doc_id == IP.II[col_idx].doc_sizes.size() - 1) {
		IP.II[col_idx].doc_sizes[doc_id] += (uint16_t)doc_size;
	}
	else {
		IP.II[col_idx].doc_sizes.push_back((uint16_t)doc_size);
	}

	for (const auto& [term_idx, tf] : terms_seen) {
		compress_uint64_differential_single(
				II.inverted_index_compressed[term_idx].doc_ids,
				doc_id,
				II.prev_doc_ids[term_idx]
				);
		add_rle_element_u8(
				II.inverted_index_compressed[term_idx].term_freqs, 
				tf
				);
		II.prev_doc_ids[term_idx] = doc_id;
	}
}
*/

void _BM25::update_progress(int line_num, int num_lines, uint16_t partition_id) {
    const int bar_width = 121;

    float percentage = static_cast<float>(line_num) / num_lines;
    int pos = bar_width * percentage;

    std::string bar;
    if (pos == bar_width) {
        bar = "[" + std::string(bar_width - 1, '=') + ">" + "]";
    } else {
        bar = "[" + std::string(pos, '=') + ">" + std::string(bar_width - pos - 1, ' ') + "]";
    }

    std::string info = std::to_string(static_cast<int>(percentage * 100)) + "% " +
                       std::to_string(line_num) + " / " + std::to_string(num_lines) + " docs read";
    std::string output = "Partition " + std::to_string(partition_id + 1) + ": " + bar + " " + info;

    {
        std::lock_guard<std::mutex> lock(progress_mutex);

        progress_bars.resize(max(progress_bars.size(), static_cast<size_t>(partition_id + 1)));
        progress_bars[partition_id] = output;

        std::cout << "\033[s";  // Save the cursor position

		// Move the cursor to the appropriate position for this partition
        std::cout << "\033[" << (partition_id + 1 + init_cursor_row) << ";1H";

        std::cout << output << std::endl;

        std::cout << "\033[u";  // Restore the cursor to the original position after updating
        std::cout << std::flush;
    }
}

void _BM25::finalize_progress_bar() {
    std::cout << "\033[" << (num_partitions + 1 + init_cursor_row) << ";1H";
	fflush(stdout);
}


/*
IIRow get_II_row(InvertedIndex* II, uint64_t term_idx) {
	IIRow row;

	row.df = get_rle_u8_row_size(II->inverted_index_compressed[term_idx].term_freqs);

	decompress_uint64(
			II->inverted_index_compressed[term_idx].doc_ids,
			row.doc_ids
			);

	// Convert doc_ids back to absolute values
	for (size_t i = 1; i < row.doc_ids.size(); ++i) {
		row.doc_ids[i] += row.doc_ids[i - 1];
	}

	// Get term frequencies
	for (size_t i = 0; i < II->inverted_index_compressed[term_idx].term_freqs.size(); ++i) {
		for (size_t j = 0; j < II->inverted_index_compressed[term_idx].term_freqs[i].num_repeats; ++j) {
			row.term_freqs.push_back(
					(uint8_t)II->inverted_index_compressed[term_idx].term_freqs[i].value
					);
		}
	}

	assert(row.doc_ids.size() == row.term_freqs.size());

	return row;
}

IIRow get_II_row_new(InvertedIndexNew* II, uint64_t term_idx) {
	IIRow row;

	row.df = II->doc_freqs[term_idx];

	// Convert doc_ids back to absolute values
	row.term_freqs.resize(row.df);
	row.doc_ids.resize(row.df);
	for (size_t i = 0; i < row.df; ++i) {
		size_t idx = II->term_offsets[term_idx] + i;
		row.term_freqs[i] = II->doc_ids[idx].tf;
		row.doc_ids[i]    = II->doc_ids[idx].doc_id;
	}

	return row;
}
*/

static inline void get_key(
		const char* line, 
		int& char_idx,
		std::string& key
		) {
	int start = char_idx;
	while (line[char_idx] != ':') {
		if (line[char_idx] == '\\') {
			char_idx += 2;
			continue;
		}
		++char_idx;
	}
	key = std::string(&line[start], char_idx - start - 1);
}

static inline void scan_to_next_key(
		const char* line, 
		int& char_idx
		) {

	while (line[char_idx] != ',') {
		if (line[char_idx] == '\\') {
			char_idx += 2;
			continue;
		}

		if (line[char_idx] == '}') return;

		if (line[char_idx] == '"') {
			++char_idx;

			// Scan to next unescaped quote
			while (line[char_idx] != '"') {
				if (line[char_idx] == '\\') {
					char_idx += 2;
					continue;
				}
				++char_idx;
			}
		}
		++char_idx;
	}
	++char_idx;
}

/*
void _BM25::write_bloom_filters(uint16_t partition_id) {
	uint32_t min_df_bloom;
	if (bloom_df_threshold <= 1.0f) {
		min_df_bloom = (uint32_t)(bloom_df_threshold * index_partitions[partition_id].num_docs);
	} else {
		min_df_bloom = (uint32_t)bloom_df_threshold / (0.5f * num_partitions);
	}
	const uint16_t TOP_K = 1000;

	for (uint16_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		BM25Partition& IP = index_partitions[partition_id];
		InvertedIndex& II = IP.II[col_idx];

		for (uint64_t idx = 0; idx < II.doc_freqs.size(); ++idx) {
			uint32_t df = II.doc_freqs[idx];
			if (df <= min_df_bloom) continue;

			// robin_hood::unordered_flat_map<uint8_t, uint64_t> tf_map;
			MAP<uint8_t, uint64_t> tf_map;
			IIRow row = get_II_row(&II, idx);

			// Construct min heap to keep track of top k term frequencies and doc ids
			std::priority_queue<
				std::pair<uint8_t, uint64_t>, 
				std::vector<std::pair<uint8_t, uint64_t>>, 
				std::greater<std::pair<uint8_t, uint64_t>>> min_heap;


			for (uint32_t i = 0; i < row.term_freqs.size(); ++i) {
				min_heap.push({row.term_freqs[i], row.doc_ids[i]});
				if (min_heap.size() > TOP_K) {
					min_heap.pop();
				}
				// distinct_term_freq_values.insert(row.term_freqs[i]);
				if (tf_map.find(row.term_freqs[i]) == tf_map.end()) {
					tf_map.insert({row.term_freqs[i], 1});
				} else {
					++(tf_map[row.term_freqs[i]]);
				}
			}

			BloomEntry bloom_entry = init_bloom_entry(bloom_fpr, tf_map, min_df_bloom);

			// partial sort TOP_K term_freqs descending. Get idxs
			bloom_entry.topk_doc_ids.reserve(min_heap.size());
			uint16_t num_topk = min_heap.size();
			for (uint16_t i = 0; i < num_topk; ++i) {
				bloom_entry.topk_doc_ids.push_back(min_heap.top().second);
				bloom_entry.topk_term_freqs.push_back(min_heap.top().first);
				min_heap.pop();
			}

			II.inverted_index_compressed[idx].doc_ids.clear();
			II.inverted_index_compressed[idx].term_freqs.clear();

			for (uint32_t i = 0; i < row.doc_ids.size(); ++i) {
				uint64_t doc_id    = row.doc_ids[i];
				size_t   term_freq = (size_t)row.term_freqs[i];
				bloom_put(bloom_entry.bloom_filters[term_freq], doc_id);
			}

			II.bloom_filters.insert({idx, bloom_entry});
		}
	}
}
*/

void _BM25::read_json(uint64_t start_byte, uint64_t end_byte, uint16_t partition_id) {
	FILE* f = reference_file_handles[partition_id];
	BM25PartitionNew* IP = &index_partitions[partition_id];

	if (fseek(f, start_byte, SEEK_SET) != 0) {
		std::cerr << "Error seeking file." << std::endl;
		std::exit(1);
	}


	// Make dir with partition_id
	std::string dir = "partition_" + std::to_string(partition_id);
	std::string cmd = "rm -rf " + dir;
	system(cmd.c_str());
	cmd = "mkdir -p " + dir;
	system(cmd.c_str());

	TokenStream* token_streams = (TokenStream*)malloc(search_cols.size() * sizeof(TokenStream));
	uint32_t* doc_freqs_capacity = (uint32_t*)malloc(search_cols.size() * sizeof(uint32_t));

	for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		doc_freqs_capacity[col_idx] = (uint32_t)(IP->num_docs * 0.1);
		IP->II[col_idx].doc_freqs = (uint32_t*)malloc(doc_freqs_capacity[col_idx] * sizeof(uint32_t));
		IP->II[col_idx].doc_sizes = (uint16_t*)malloc(IP->num_docs * sizeof(uint16_t));

		std::string filename = dir + "/" + "col_" + std::to_string(col_idx) + ".txt";
		init_token_stream(&token_streams[col_idx], filename);
	}

	// Reset file pointer to beginning
	fseek(f, start_byte, SEEK_SET);

	// Read the file line by line
	char*    line = NULL;
	size_t   len = 0;
	ssize_t  read;
	uint64_t line_num = 0;
	uint64_t byte_offset = start_byte;

	const uint32_t UPDATE_INTERVAL = max(1, IP->num_docs / 1000);
	while ((read = json_getline(&line, &len, f)) != -1) {

		if (byte_offset >= end_byte) {
			break;
		}

		if (line_num % UPDATE_INTERVAL == 0) update_progress(line_num, IP->num_docs, partition_id);
		if (strlen(line) == 0) {
			std::cout << "Empty line found" << std::endl;
			std::exit(1);
		}

		std::string key = "";
		bool found = false;

		// First char is always `{`
		int char_idx = 1;
		while (true) {
			start:
				while (line[char_idx] == ' ') ++char_idx;

				// Found key. Match against search_col.
				if (line[char_idx] == '"') {
					// Iter over quote.
					++char_idx;

					// Get key. char_idx will now be on a ':'.
					get_key(line, char_idx, key); ++char_idx;

					uint16_t search_col_idx = 0;
					for (const auto& search_col : search_cols) {
						if (key == search_col) {
							found = true;

							// Go to first char of value.
							while (line[char_idx] == ' ') ++char_idx;

							if (line[char_idx] != '"') {
								// Assume null. Must be string values.
								key.clear();
								++search_col_idx;
								scan_to_next_key(line, char_idx);
								goto start;
							}

							// Iter over quote.
							++char_idx;

							char_idx += process_doc_partition_json(
									&line[char_idx], 
									'"', 
									&token_streams[search_col_idx],
									line_num, 
									partition_id,
									search_col_idx,
									&doc_freqs_capacity[search_col_idx]
									); ++char_idx;
							scan_to_next_key(line, char_idx);
							key.clear();
							++search_col_idx;
							goto start;
						}
					}

					key.clear();
					scan_to_next_key(line, char_idx);
					++search_col_idx;
				}
				else if (line[char_idx] == '}') {
					if (!found) {
						std::cout << "Search field not found on line: " << line_num << std::endl;
						std::cout << std::flush;
						std::exit(1);
					}
					// Success. Break.
					break;
				}
				else if (char_idx > 1048576 || char_idx > (int)strlen(line)) {
					std::cout << "Search field not found on line: " << line_num << std::endl;
					std::cout << "Line: " << line << std::endl;
					std::cout << std::flush;
					std::exit(1);
				}
				else {
					std::cerr << "Invalid json." << std::endl;
					std::cout << "Line: " << line << std::endl;
					std::cout << &line[char_idx] << std::endl;
					std::cout << char_idx << std::endl;
					std::cout << std::flush;
					std::exit(1);
				}
		}

			if (line_num % UPDATE_INTERVAL == 0) update_progress(line_num, IP->num_docs, partition_id);

		++line_num;
	}

	update_progress(line_num, IP->num_docs, partition_id);
	free(line);

	// Calc avg_doc_size
	for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		double avg_doc_size = 0;
		for (size_t idx = 0; idx < IP->num_docs; ++idx) {
			avg_doc_size += (double)IP->II[col_idx].doc_sizes[idx];
		}
		IP->II[col_idx].avg_doc_size = (float)(avg_doc_size / IP->num_docs);
	}


	// TODO: Process token stream and build II and bloom filters.
	for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		IP->II[col_idx].num_terms = IP->unique_term_mappings[col_idx].size();
		IP->II[col_idx].num_docs  = IP->num_docs;
		IP->II[col_idx].avg_doc_size = IP->II[col_idx].avg_doc_size;

		read_token_stream(&IP->II[col_idx], &token_streams[col_idx]);
	}

	free(token_streams);
	free(doc_freqs_capacity);

	// delete dir
	std::string rm_cmd = "rm -rf " + dir;
	system(rm_cmd.c_str());
}

void _BM25::read_csv_rfc_4180(uint64_t start_byte, uint64_t end_byte, uint16_t partition_id) {
	FILE* f = reference_file_handles[partition_id];
	BM25PartitionNew* IP = &index_partitions[partition_id];

	// Reset file pointer to beginning
	if (fseek(f, start_byte, SEEK_SET) != 0) {
		std::cerr << "Error seeking file." << std::endl;
		std::exit(1);
	}

	// Read the file line by line
	char*    line = NULL;
	uint64_t line_num = 0;

	// Make dir with partition_id
	std::string dir = "partition_" + std::to_string(partition_id);
	std::string cmd = "rm -rf " + dir;
	system(cmd.c_str());
	cmd = "mkdir -p " + dir;
	system(cmd.c_str());

	TokenStream* token_streams = (TokenStream*)malloc(search_cols.size() * sizeof(TokenStream));
	uint32_t* doc_freqs_capacity = (uint32_t*)malloc(search_cols.size() * sizeof(uint32_t));

	for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		init_inverted_index_new(&IP->II[col_idx]);

		doc_freqs_capacity[col_idx] = (uint32_t)(IP->num_docs * 0.1);
		IP->II[col_idx].doc_freqs = (uint32_t*)malloc(
				doc_freqs_capacity[col_idx] * sizeof(uint32_t)
				);
		IP->II[col_idx].doc_sizes = (uint16_t*)malloc(IP->num_docs * sizeof(uint16_t));

		std::string filename = dir + "/" + "col_" + std::to_string(col_idx) + ".txt";
		init_token_stream(&token_streams[col_idx], filename);
	}

	std::string doc = "";

	char end_delim = ',';

	uint64_t current_offset;
	uint64_t next_offset;
	
	assert(IP->num_docs != 0);

	line = (char*)malloc(1048576);

	const uint32_t UPDATE_INTERVAL = max(1, IP->num_docs / 1000);
	while (line_num < IP->num_docs) {
		current_offset = IP->line_offsets[line_num];

		fseek(f, current_offset, SEEK_SET);
		if (line_num == IP->num_docs - 1) {
			next_offset = end_byte;
			fread(line, 1, next_offset - current_offset, f);
		} else {
			next_offset = IP->line_offsets[line_num + 1];
			fread(line, 1, next_offset - current_offset, f);
		}


		if (line_num % UPDATE_INTERVAL == 0) update_progress(line_num, IP->num_docs, partition_id);

		int char_idx = 0;
		int col_idx  = 0;
		size_t _search_col_idx = 0;
		for (size_t sc_idx = 0; sc_idx < search_col_idxs.size(); ++sc_idx) {
			int search_col_idx = search_col_idxs[sc_idx];

			if (search_col_idx == (int)columns.size() - 1) {
				end_delim = '\n';
			}
			else {
				end_delim = ',';
			}

			// Iterate of line chars until we get to relevant column.
			while (col_idx != search_col_idx) {
				if (line[char_idx] == '\n') {
					printf("Newline found before end.\n");
					printf("Search col idx: %d\n", search_col_idx);
					printf("Col idx: %d\n", col_idx);
					printf("Char idx: %d\n", char_idx);
					printf("Line: %s", line);
					printf("Line num: %lu\n", line_num);
					printf("Partition id: %d\n", partition_id);
					exit(1);
				}

				if (line[char_idx] == '"') {
					// Skip to next unescaped quote
					++char_idx;

					while (1) {
						if (line[char_idx] == '"') {
							if (line[char_idx + 1] == '"') {
								char_idx += 2;
								continue;
							} 
							else {
								++char_idx;
								break;
							}
						}
						++char_idx;
					}
				}

				if (line[char_idx] == ',') ++col_idx;
				++char_idx;
			}
			++col_idx;

			// Split by commas not inside double quotes
			if (line[char_idx] == '"') {
				++char_idx;
				char_idx += _process_doc_partition_rfc_4180_quoted_v2(
					&line[char_idx],
					&token_streams[_search_col_idx],
					line_num,
					partition_id,
					_search_col_idx,
					&doc_freqs_capacity[_search_col_idx]
					) + 1;
				++_search_col_idx;
				continue;
			}

			char_idx += process_doc_partition_rfc_4180_v2(
				&line[char_idx], 
				end_delim,
				&token_streams[_search_col_idx],
				line_num, 
				partition_id,
				_search_col_idx,
				&doc_freqs_capacity[_search_col_idx]
				);
			++_search_col_idx;
		}
		++line_num;
	}

	free(line);

	// Flush remaining tokens
	for (size_t col = 0; col < search_cols.size(); ++col) {
		flush_token_stream(&token_streams[col]);
	}

	if (!DEBUG) update_progress(line_num, IP->num_docs, partition_id);

	// Calc avg_doc_size
	for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		double avg_doc_size = 0;
		for (size_t idx = 0; idx < IP->num_docs; ++idx) {
			avg_doc_size += (double)IP->II[col_idx].doc_sizes[idx];
		}
		IP->II[col_idx].avg_doc_size = (float)(avg_doc_size / IP->num_docs);
	}

	// TODO: Process token stream and build II and bloom filters.
	for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		IP->II[col_idx].num_terms = IP->unique_term_mappings[col_idx].size();
		IP->II[col_idx].num_docs  = IP->num_docs;
		IP->II[col_idx].avg_doc_size = IP->II[col_idx].avg_doc_size;

		read_token_stream(&IP->II[col_idx], &token_streams[col_idx]);
	}

	free(token_streams);
	free(doc_freqs_capacity);

	// delete dir
	std::string rm_cmd = "rm -rf " + dir;
	system(rm_cmd.c_str());
}



void _BM25::read_in_memory(
		std::vector<std::vector<std::string>>& documents,
		uint64_t start_idx, 
		uint64_t end_idx, 
		uint16_t partition_id
		) {
	BM25PartitionNew* IP = &index_partitions[partition_id];

	IP->num_docs = end_idx - start_idx;

	// Make dir with partition_id
	std::string dir = "partition_" + std::to_string(partition_id);
	std::string cmd = "rm -rf " + dir;
	system(cmd.c_str());
	cmd = "mkdir -p " + dir;
	system(cmd.c_str());

	TokenStream* token_streams = (TokenStream*)malloc(search_cols.size() * sizeof(TokenStream));
	uint32_t* doc_freqs_capacity = (uint32_t*)malloc(search_cols.size() * sizeof(uint32_t));

	for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		doc_freqs_capacity[col_idx] = (uint32_t)(IP->num_docs * 0.1);
		IP->II[col_idx].doc_freqs = (uint32_t*)malloc(doc_freqs_capacity[col_idx] * sizeof(uint32_t));
		IP->II[col_idx].doc_sizes = (uint16_t*)malloc(IP->num_docs * sizeof(uint16_t));

		std::string filename = dir + "/" + "col_" + std::to_string(col_idx) + ".txt";
		init_token_stream(&token_streams[col_idx], filename);

		init_inverted_index_new(&IP->II[col_idx]);
	}

	uint32_t cntr = 0;
	const uint32_t UPDATE_INTERVAL = max(1, IP->num_docs / 1000);

	for (uint64_t line_num = start_idx; line_num < end_idx; ++line_num) {
		if (cntr % UPDATE_INTERVAL == 0) update_progress(cntr, IP->num_docs, partition_id);

		for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
			std::string& doc = documents[line_num][col_idx];
			process_doc_partition_rfc_4180_v2(
				(doc + "\n").c_str(),
				'\n',
				&token_streams[col_idx],
				cntr, 
				partition_id,
				col_idx,
				&doc_freqs_capacity[col_idx]
				);
		}
		++cntr;
	}
	if (!DEBUG) update_progress(cntr + 1, IP->num_docs, partition_id);

	// Calc avg_doc_size
	for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		double avg_doc_size = 0;
		for (size_t idx = 0; idx < IP->num_docs; ++idx) {
			avg_doc_size += (double)IP->II[col_idx].doc_sizes[idx];
		}
		IP->II[col_idx].avg_doc_size = (float)(avg_doc_size / IP->num_docs);
	}

	// TODO: Process token stream and build II and bloom filters.
	for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		IP->II[col_idx].num_terms = IP->unique_term_mappings[col_idx].size();
		IP->II[col_idx].num_docs  = IP->num_docs;
		IP->II[col_idx].avg_doc_size = IP->II[col_idx].avg_doc_size;

		read_token_stream(&IP->II[col_idx], &token_streams[col_idx]);
	}

	free(token_streams);
	free(doc_freqs_capacity);

	// delete dir
	std::string rm_cmd = "rm -rf " + dir;
	system(rm_cmd.c_str());
}


std::vector<std::pair<std::string, std::string>> _BM25::get_csv_line(
		uint32_t line_num, 
		uint16_t partition_id
		) {
	FILE* f = reference_file_handles[partition_id];
	BM25PartitionNew* IP = &index_partitions[partition_id];

	// seek from FILE* reference_file which is already open
	fseek(f, IP->line_offsets[line_num], SEEK_SET);
	char*   line = NULL;
	size_t  len  = 0;
	ssize_t read = rfc4180_getline(&line, &len, f);

	// Create effective json by combining column names with values split by commas
	std::vector<std::pair<std::string, std::string>> row;
	std::string cell;
	size_t col_idx = 0;
	size_t i = 0;

	while (i < (size_t)read - 1) {

		if (line[i] == '"') {
			// Scan to next unescaped quote
			++i;
			while (1) {
				if (line[i] == '"') {
					if (line[i + 1] == '"') {
						cell += '"';
						i += 2;
						continue;
					} 
					else {
						++i;
						break;
					}
				}
				cell += line[i];
				++i;
			}
		}

		if (line[i] == ',') {
			row.emplace_back(columns[col_idx], cell);
			cell.clear();
			++col_idx;
		}
		else {
			cell += line[i];
		}
		++i;
	}
	row.emplace_back(columns[col_idx], cell);
	return row;
}


std::vector<std::pair<std::string, std::string>> _BM25::get_json_line(
		uint32_t line_num, 
		uint16_t partition_id
		) {
	FILE* f = reference_file_handles[partition_id];
	BM25PartitionNew* IP = &index_partitions[partition_id];

	// seek from FILE* reference_file which is already open
	fseek(f, IP->line_offsets[line_num], SEEK_SET);
	char* line = NULL;
	size_t len = 0;
	ssize_t size = getline(&line, &len, f);
	if (size == -1) {
		std::cerr << "Error reading line." << std::endl;
		std::exit(1);
	}

	// Create effective json by combining column names with values split by commas
	std::vector<std::pair<std::string, std::string>> row;

	std::string first  = "";
	std::string second = "";

	if (line[1] == '}') {
		return row;
	}

	size_t char_idx = 2;
	while (true) {
		while (line[char_idx] != '"') {
			if (line[char_idx] == '\\') {
				++char_idx;
				first += line[char_idx];
				++char_idx;
				continue;
			}
			first += line[char_idx];
			++char_idx;
		}
		char_idx += 2;

		// Go to first char of value.
		while (line[char_idx] == '"' || line[char_idx] == ' ') {
			++char_idx;
		}

		while (line[char_idx] != '}' || line[char_idx] != '"' || line[char_idx] != ',') {
			if (line[char_idx] == '\\') {
				++char_idx;
				second += line[char_idx];
				++char_idx;
				continue;
			}
			else if (line[char_idx] == '}') {
				second += line[char_idx];
				row.emplace_back(first, second);
				return row;
			}
			second += line[char_idx];
			++char_idx;
		}
		++char_idx;
		if (line[char_idx] == '}') {
			return row;
		}
	}
	return row;
}

/*
void _BM25::save_index_partition(
		std::string db_dir,
		uint16_t partition_id
		) {
	std::string UNIQUE_TERM_MAPPING_PATH = db_dir + "/unique_term_mapping.bin";
	std::string INVERTED_INDEX_PATH 	 = db_dir + "/inverted_index.bin";
	std::string DOC_SIZES_PATH 		     = db_dir + "/doc_sizes.bin";
	std::string LINE_OFFSETS_PATH 		 = db_dir + "/line_offsets.bin";

	BM25Partition& IP = index_partitions[partition_id];


	for (uint16_t col_idx = 0; col_idx < search_col_idxs.size(); ++col_idx) {
		serialize_robin_hood_flat_map_string_u32(
				IP.unique_term_mapping[col_idx],
				UNIQUE_TERM_MAPPING_PATH + "_" + std::to_string(partition_id) + "_" + std::to_string(col_idx)
				);
		serialize_inverted_index(
				IP.II[col_idx], 
				INVERTED_INDEX_PATH + "_" + std::to_string(partition_id) + "_" + std::to_string(col_idx)
				);
	}

	std::vector<uint8_t> compressed_line_offsets;
	compressed_line_offsets.reserve(IP.line_offsets.size() * 2);
	compress_uint64(IP.line_offsets, compressed_line_offsets);

	serialize_vector_u8(
			compressed_line_offsets, LINE_OFFSETS_PATH + "_" + std::to_string(partition_id)
			);

	// Save doc_freqs
	for (uint16_t col_idx = 0; col_idx < search_col_idxs.size(); ++col_idx) {
		serialize_vector_u32(
				IP.II[col_idx].doc_freqs, 
				db_dir + "/doc_freqs_" + std::to_string(partition_id) + "_" + std::to_string(col_idx)
				);
	}
}

void _BM25::load_index_partition(
		std::string db_dir,
		uint16_t partition_id
		) {
	std::string UNIQUE_TERM_MAPPING_PATH = db_dir + "/unique_term_mapping.bin";
	std::string INVERTED_INDEX_PATH 	 = db_dir + "/inverted_index.bin";
	std::string DOC_SIZES_PATH 		     = db_dir + "/doc_sizes.bin";
	std::string LINE_OFFSETS_PATH 		 = db_dir + "/line_offsets.bin";

	BM25Partition& IP = index_partitions[partition_id];
	IP.unique_term_mapping.resize(search_col_idxs.size());
	IP.II.resize(search_col_idxs.size());

	for (uint16_t col_idx = 0; col_idx < search_col_idxs.size(); ++col_idx) {
		deserialize_robin_hood_flat_map_string_u32(
				IP.unique_term_mapping[col_idx],
				UNIQUE_TERM_MAPPING_PATH + "_" + std::to_string(partition_id) + "_" + std::to_string(col_idx)
				);
		deserialize_inverted_index(
				IP.II[col_idx], 
				INVERTED_INDEX_PATH + "_" + std::to_string(partition_id) + "_" + std::to_string(col_idx)
				);
	}

	std::vector<uint8_t> compressed_line_offsets;
	deserialize_vector_u8(
			compressed_line_offsets, LINE_OFFSETS_PATH + "_" + std::to_string(partition_id)
			);

	decompress_uint64(compressed_line_offsets, IP.line_offsets);
	printf("Line offsets size: %lu\n", IP.line_offsets.size());

	// Load doc_freqs
	for (uint16_t col_idx = 0; col_idx < search_col_idxs.size(); ++col_idx) {
		deserialize_vector_u32(
				IP.II[col_idx].doc_freqs, 
				db_dir + "/doc_freqs_" + std::to_string(partition_id) + "_" + std::to_string(col_idx)
				);
	}

	IP.num_docs = IP.II[0].doc_sizes.size();
}

void _BM25::save_to_disk(const std::string& db_dir) {
	auto start = std::chrono::high_resolution_clock::now();

	if (access(db_dir.c_str(), F_OK) != -1) {
		// Remove the directory if it exists
		std::string command = "rm -r " + db_dir;
		int success = system(command.c_str());
		if (success != 0) {
			std::cerr << "Error removing directory.\n";
			std::exit(1);
		}

		// Create the directory
		command = "mkdir " + db_dir;
		success = system(command.c_str());
		if (success != 0) {
			std::cerr << "Error creating directory.\n";
			std::exit(1);
		}
	}
	else {
		// Create the directory if it does not exist
		std::string command = "mkdir " + db_dir;
		int success = system(command.c_str());
		if (success != 0) {
			std::cerr << "Error creating directory.\n";
			std::exit(1);
		}
	}

	// Join paths
	std::string METADATA_PATH = db_dir + "/metadata.bin";

	std::vector<std::thread> threads;
	for (uint16_t partition_id = 0; partition_id < num_partitions; ++partition_id) {
		threads.push_back(std::thread(&_BM25::save_index_partition, this, db_dir, partition_id));
	}

	for (auto& thread : threads) {
		thread.join();
	}

	// Save partition boundaries
	std::string PARTITION_BOUNDARY_PATH = db_dir + "/partition_boundaries.bin";
	serialize_vector_u64(partition_boundaries, PARTITION_BOUNDARY_PATH);

	// Serialize smaller members.
	std::ofstream out_file(METADATA_PATH, std::ios::binary);
	if (!out_file) {
		std::cerr << "Error opening file for writing.\n";
		return;
	}

	// Write basic types directly
	out_file.write(reinterpret_cast<const char*>(&num_docs), sizeof(num_docs));
	out_file.write(reinterpret_cast<const char*>(&bloom_df_threshold), sizeof(bloom_df_threshold));
	out_file.write(reinterpret_cast<const char*>(&bloom_fpr), sizeof(bloom_fpr));
	out_file.write(reinterpret_cast<const char*>(&k1), sizeof(k1));
	out_file.write(reinterpret_cast<const char*>(&b), sizeof(b));
	out_file.write(reinterpret_cast<const char*>(&num_partitions), sizeof(num_partitions));

	// Write std::string
	size_t filename_length = filename.size();
	out_file.write(reinterpret_cast<const char*>(&filename_length), sizeof(filename_length));
	out_file.write(filename.data(), filename_length);

	// Write enum as int
	int file_type_int = static_cast<int>(file_type);
	out_file.write(reinterpret_cast<const char*>(&file_type_int), sizeof(file_type_int));

	// Write std::vector<std::string>
	size_t columns_size = columns.size();
	out_file.write(reinterpret_cast<const char*>(&columns_size), sizeof(columns_size));
	for (const auto& col : columns) {
		size_t col_length = col.size();
		out_file.write(reinterpret_cast<const char*>(&col_length), sizeof(col_length));
		out_file.write(col.data(), col_length);
	}

	// Write search_cols std::vector<std::string>
	columns_size = search_cols.size();
	out_file.write(reinterpret_cast<const char*>(&columns_size), sizeof(columns_size));
	for (const auto& search_col : search_cols) {
		size_t search_col_length = search_col.size();
		out_file.write(reinterpret_cast<const char*>(&search_col_length), sizeof(search_col_length));
		out_file.write(search_col.data(), search_col_length);
	}

	for (const auto& search_col_idx : search_col_idxs) {
		out_file.write(reinterpret_cast<const char*>(&search_col_idx), sizeof(search_col_idx));
	}

	out_file.write(reinterpret_cast<const char*>(&header_bytes), sizeof(header_bytes));

	out_file.close();

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed_seconds = end - start;

	if (DEBUG) {
		std::cout << "Saved in " << elapsed_seconds.count() << "s" << std::endl;
	}
}

void _BM25::load_from_disk(const std::string& db_dir) {
	auto start = std::chrono::high_resolution_clock::now();

	// Join paths
	std::string UNIQUE_TERM_MAPPING_PATH = db_dir + "/unique_term_mapping.bin";
	std::string INVERTED_INDEX_PATH 	 = db_dir + "/inverted_index.bin";
	std::string LINE_OFFSETS_PATH 		 = db_dir + "/line_offsets.bin";
	std::string METADATA_PATH 			 = db_dir + "/metadata.bin";
	std::string PARTITION_BOUNDARY_PATH  = db_dir + "/partition_boundaries.bin";

	// Load smaller members.
	std::ifstream in_file(METADATA_PATH, std::ios::binary);
    if (!in_file) {
        std::cerr << "Error opening file for reading.\n";
        return;
    }

    // Read basic types directly.
    in_file.read(reinterpret_cast<char*>(&num_docs), sizeof(num_docs));
    in_file.read(reinterpret_cast<char*>(&bloom_df_threshold), sizeof(bloom_df_threshold));
    in_file.read(reinterpret_cast<char*>(&bloom_fpr), sizeof(bloom_fpr));
    in_file.read(reinterpret_cast<char*>(&k1), sizeof(k1));
    in_file.read(reinterpret_cast<char*>(&b), sizeof(b));
	in_file.read(reinterpret_cast<char*>(&num_partitions), sizeof(num_partitions));

	// Read and prep file data.
    size_t filename_length;
    in_file.read(reinterpret_cast<char*>(&filename_length), sizeof(filename_length));
    filename.resize(filename_length);
    in_file.read(&filename[0], filename_length);

	// read fn_file contents into filename 
	FILE* f = fopen(filename.c_str(), "r");
	if (f == nullptr) {
		printf("Error opening file: %s\n", filename.c_str());
		exit(1);
	}
	char buf[1024];
	char* res = fgets(buf, 1024, f);
	if (res == nullptr) {
		printf("Error reading file: %s\n", filename.c_str());
		exit(1);
	}
	fclose(f);

	if (filename == "in_memory") {
		file_type = IN_MEMORY;
	} else if (filename.find(".json") != std::string::npos) {
		file_type = JSON;
	} else if (filename.find(".csv") != std::string::npos) {
		file_type = CSV;
	} else {
		std::cerr << "Error: file type not supported" << std::endl;
		exit(1);
	}

	if (file_type == IN_MEMORY) {
		return;
	}

	// Open the reference file
	for (uint16_t i = 0; i < num_partitions; i++) {
		FILE* ref_f = fopen(filename.c_str(), "r");
		if (ref_f == nullptr) {
			printf("Error opening file: %s\n", filename.c_str());
			printf("Partition id: %d\n", i);
			exit(1);
		}
		reference_file_handles.push_back(ref_f);
	}

	// Load partition boundaries
	deserialize_vector_u64(partition_boundaries, PARTITION_BOUNDARY_PATH);

	index_partitions.clear();
	index_partitions.resize(num_partitions);

    // Read enum as int
    int file_type_int;
    in_file.read(reinterpret_cast<char*>(&file_type_int), sizeof(file_type_int));
    file_type = static_cast<SupportedFileTypes>(file_type_int);

    // Read std::vector<std::string>
    size_t columns_size;
    in_file.read(reinterpret_cast<char*>(&columns_size), sizeof(columns_size));
    columns.resize(columns_size);
    for (auto& col : columns) {
        size_t col_length;
        in_file.read(reinterpret_cast<char*>(&col_length), sizeof(col_length));
        col.resize(col_length);
        in_file.read(&col[0], col_length);
    }

    // Read search_cols std::vector<std::string>
	in_file.read(reinterpret_cast<char*>(&columns_size), sizeof(columns_size));
	search_cols.resize(columns_size);
	for (auto& search_col : search_cols) {
		size_t search_col_length;
		in_file.read(reinterpret_cast<char*>(&search_col_length), sizeof(search_col_length));
		search_col.resize(search_col_length);
		in_file.read(&search_col[0], search_col_length);
	}

	search_col_idxs.resize(search_cols.size());
	for (auto& search_col_idx : search_col_idxs) {
		in_file.read(reinterpret_cast<char*>(&search_col_idx), sizeof(search_col_idx));
	}

	in_file.read(reinterpret_cast<char*>(&header_bytes), sizeof(header_bytes));

    in_file.close();

	std::vector<std::thread> threads;

	for (uint16_t partition_id = 0; partition_id < num_partitions; ++partition_id) {
		threads.push_back(std::thread(&_BM25::load_index_partition, this, db_dir, partition_id));
	}

	for (auto& thread : threads) {
		thread.join();
	}

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed_seconds = end - start;

	printf("Loaded in %fs\n", elapsed_seconds.count()); fflush(stdout);
}
*/

_BM25::_BM25(
		std::string filename,
		std::vector<std::string> search_cols,
		float  bloom_df_threshold,
		double bloom_fpr,
		float  k1,
		float  b,
		uint16_t num_partitions,
		const std::vector<std::string>& _stop_words
		) : bloom_df_threshold(bloom_df_threshold),
			bloom_fpr(bloom_fpr),
			k1(k1), 
			b(b),
			num_partitions(num_partitions),
			search_cols(search_cols), 
			filename(filename) {


	for (const std::string& stop_word : _stop_words) {
		stop_words.insert(stop_word);
	}

	// Open file handles
	for (uint16_t i = 0; i < num_partitions; ++i) {
		FILE* f = fopen(filename.c_str(), "r");
		if (f == NULL) {
			std::cerr << "Unable to open file: " << filename << std::endl;
			exit(1);
		}
		reference_file_handles.push_back(f);
	}

	auto overall_start = std::chrono::high_resolution_clock::now();

	std::vector<std::thread> threads;

	// num_docs = 0;

	progress_bars.resize(num_partitions);

	// Read file to get documents, line offsets, and columns
	if (filename.substr(filename.size() - 3, 3) == "csv") {

		proccess_csv_header();
		determine_partition_boundaries_csv_rfc_4180();

		init_terminal();

		// Launch num_partitions threads to read csv file
		for (uint16_t i = 0; i < num_partitions; ++i) {
			threads.push_back(std::thread(
				[this, i] {
					read_csv_rfc_4180(partition_boundaries[i], partition_boundaries[i + 1], i);
					// write_bloom_filters(i);
				}
			));
		}

		file_type = CSV;
	}
	else if (filename.substr(filename.size() - 4, 4) == "json") {
		header_bytes = 0;
		determine_partition_boundaries_json();

		init_terminal();

		// Launch num_partitions threads to read json file
		for (uint16_t i = 0; i < num_partitions; ++i) {
			threads.push_back(std::thread(
				[this, i] {
					read_json(partition_boundaries[i], partition_boundaries[i + 1], i);
					// write_bloom_filters(i);
				}
			));
		}
		file_type = JSON;
	}
	else {
		std::cout << "Only csv and json files are supported." << std::endl;
		std::exit(1);
	}


	for (auto& thread : threads) {
		thread.join();
	}

	num_docs = 0;
	for (size_t i = 0; i < num_partitions; ++i) {
		num_docs += index_partitions[i].num_docs;
	}

	if (!DEBUG) finalize_progress_bar();

	uint64_t total_size = 0;
	uint32_t unique_terms_found = 0;
	for (size_t i = 0; i < num_partitions; ++i) {
		BM25PartitionNew* IP = &index_partitions[i];

		uint64_t part_size = 0;
		for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
			unique_terms_found += IP->unique_term_mappings[col_idx].size();
			total_size += calc_inverted_index_size(&IP->II[col_idx]);
		}
		total_size += part_size;

	}
	total_size /= 1024 * 1024;
	uint64_t vocab_size = unique_terms_found * (4 + 32 + 5 + 1) / 1048576;
	uint64_t line_offsets_size = num_docs * 8 / 1048576;
	uint64_t inverted_index_size = total_size;
	total_size = vocab_size + line_offsets_size + inverted_index_size;

	printf("Total size of vocab mappings: ~%luMB\n", vocab_size);
	printf("Total size of line offsets: %luMB\n", line_offsets_size);
	printf("Total size of inverted indexes: %luMB\n", inverted_index_size);
	printf("--------------------------------------\n");
	printf("Approx total in-memory size:    %luMB\n\n", total_size);

	printf("Total number of documents:      %lu\n", num_docs);
	printf("Total number of unique terms:   %u\n", unique_terms_found);

	auto read_end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> read_elapsed_seconds = read_end - overall_start;

	printf("Read file in %fs\n", read_elapsed_seconds.count());
	printf("KDocs/s: %lu\n", (uint64_t)(num_docs * 0.001f / read_elapsed_seconds.count()));
}


_BM25::_BM25(
		std::vector<std::vector<std::string>>& documents,
		float  bloom_df_threshold,
		double bloom_fpr,
		float  k1,
		float  b,
		uint16_t num_partitions,
		const std::vector<std::string>& _stop_words
		) : bloom_df_threshold(bloom_df_threshold),
			bloom_fpr(bloom_fpr),
			k1(k1), 
			b(b),
			num_partitions(num_partitions) {

	auto overall_start = std::chrono::high_resolution_clock::now();
	
	for (const std::string& stop_word : _stop_words) {
		stop_words.insert(stop_word);
	}

	filename = "in_memory";
	file_type = IN_MEMORY;

	num_docs = documents.size();

	search_cols.resize(documents[0].size());
	search_col_idxs.resize(documents[0].size());

	partition_boundaries.resize(num_partitions + 1);

	index_partitions = (BM25PartitionNew*)malloc(num_partitions * sizeof(BM25PartitionNew));

	for (uint16_t i = 0; i < num_partitions; ++i) {
		partition_boundaries[i] = (uint64_t)i * (num_docs / num_partitions);
		init_bm25_partition_new(
				&index_partitions[i],
				num_docs / num_partitions,
				search_cols.size()
				);
	}
	partition_boundaries[num_partitions] = num_docs;

	progress_bars.resize(num_partitions);
	bool is_terminal = isatty(fileno(stdout));

	int col;
	if (is_terminal) {
		get_cursor_position(init_cursor_row, col);
		get_terminal_size(terminal_height, col);

		if (terminal_height - init_cursor_row < num_partitions + 1) {
			// Scroll and reposition cursor
			std::cout << "\x1b[" << num_partitions + 1 << "S";
			init_cursor_row -= num_partitions + 1;
		}
	}

	std::vector<std::thread> threads;
	for (uint16_t i = 0; i < num_partitions; ++i) {
		threads.push_back(std::thread(
			[this, &documents, i] {
				read_in_memory(
						documents, 
						partition_boundaries[i], 
						partition_boundaries[i + 1], 
						i
						);
				// write_bloom_filters(i);
			}
		));
	}

	for (auto& thread : threads) {
		thread.join();
	}

	if (!DEBUG) finalize_progress_bar();

	uint64_t total_size = 0;
	uint32_t unique_terms_found = 0;
	/*
	for (uint16_t i = 0; i < num_partitions; ++i) {
		BM25Partition& IP = index_partitions[i];

		uint64_t part_size = 0;
		for (uint16_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
			for (const auto& row : IP.II[col_idx].inverted_index_compressed) {
				part_size += sizeof(uint8_t) * row.doc_ids.size();
				part_size += sizeof(RLEElement_u8) * row.term_freqs.size();
			}
			unique_terms_found += IP.unique_term_mapping[col_idx].size();
		}
		total_size += part_size;

	}
	total_size /= 1024 * 1024;
	*/

	uint64_t vocab_size = unique_terms_found * (4 + 5 + 1) / 1048576;
	uint64_t line_offsets_size = num_docs * 8 / 1048576;
	uint64_t doc_sizes_size = num_docs * 2 * search_cols.size() / 1048576;
	uint64_t inverted_index_size = total_size;
	total_size = vocab_size + line_offsets_size + doc_sizes_size + inverted_index_size;

	std::cout << "Total size of vocab mappings:  ~" << vocab_size << "MB" << std::endl;
	std::cout << "Total size of line offsets:     " << line_offsets_size << "MB" << std::endl;
	std::cout << "Total size of doc sizes:        " << doc_sizes_size << "MB" << std::endl;
	std::cout << "Total size of inverted indexes: " << inverted_index_size << "MB" << std::endl;
	std::cout << "--------------------------------------" << std::endl;
	std::cout << "Approx total in-memory size:    " << total_size << "MB" << std::endl << std::endl;

	std::cout << "Total number of documents:      " << num_docs << std::endl;
	std::cout << "Total number of unique terms:   " << unique_terms_found << std::endl;

	auto read_end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> read_elapsed_seconds = read_end - overall_start;

	printf("Read file in %fs\n", read_elapsed_seconds.count());
	printf("KDocs/s: %lu\n", (uint64_t)(num_docs * 0.001f / read_elapsed_seconds.count()));
}

inline float _BM25::_compute_bm25(
		uint64_t doc_id,
		float tf,
		float idf,
		uint16_t col_idx,
		uint16_t partition_id
		) {
	BM25PartitionNew IP = index_partitions[partition_id];

	float weigted_doc_size = IP.II[col_idx].doc_sizes[doc_id] / IP.II[col_idx].avg_doc_size;
	return idf * tf / (tf + k1 * (1 - b + b * weigted_doc_size));
}

void _BM25::add_query_term(
		std::string& substr,
		std::vector<std::vector<uint64_t>>& term_idxs,
		uint16_t partition_id
		) {
	BM25PartitionNew* IP = &index_partitions[partition_id];

	for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		MAP<std::string, uint32_t>& vocab = IP->unique_term_mappings[col_idx];

		if (vocab.find(substr) == vocab.end()) {
			continue;
		}

		if (stop_words.find(substr) != stop_words.end()) {
			continue;
		}

		term_idxs[col_idx].push_back(vocab[substr]);
	}
	substr.clear();
}


TermType _BM25::add_query_term_bloom(
		std::string& substr,
		std::vector<std::vector<uint64_t>>& term_idxs,
		std::vector<MAP<uint64_t, BloomEntry>>& bloom_entries,
		uint16_t partition_id,
		uint16_t col_idx
		) {
	BM25PartitionNew* IP = &index_partitions[partition_id];

	MAP<std::string, uint32_t>& vocab = IP->unique_term_mappings[col_idx];

	auto it = vocab.find(substr);
	if (it == vocab.end()) {
		substr.clear();
		term_idxs[col_idx].push_back(UINT64_MAX);
		return UNKNOWN;
	}

	if (stop_words.find(substr) != stop_words.end()) {
		substr.clear();
		term_idxs[col_idx].push_back(UINT64_MAX);
		return UNKNOWN;
	}
	substr.clear();
	term_idxs[col_idx].push_back(it->second);

	// TODO: Fix
	/*
	auto it2 = IP.II[col_idx].bloom_filters.find(it->second);
	if (it2 == IP.II[col_idx].bloom_filters.end()) {
		return LOW_DF;
	}
	else {
		bloom_entries[col_idx][it->second] = it2->second;
		return HIGH_DF;
	}
	*/
	return LOW_DF;
}


static inline uint64_t get_doc_id(
		StandardEntry& IIE,
		uint32_t& current_idx,
		uint64_t& prev_doc_id
		) {
	uint64_t doc_id;

	current_idx += decompress_uint64_differential_single_bytes(
			&(IIE.doc_ids[current_idx]),
			doc_id,
			prev_doc_id
			);

	prev_doc_id = doc_id;

	return doc_id;
}

static inline std::pair<uint64_t, uint16_t> pop_replace_minheap(
		std::priority_queue<
			std::pair<uint64_t, uint16_t>,
			std::vector<std::pair<uint64_t, uint16_t>>,
			_compare_64_16>& min_heap,
		MAP<uint16_t, StandardEntry*>& II_streams,
		std::vector<uint32_t>& stream_idxs,
		std::vector<uint64_t>& prev_doc_ids
		) {

	std::pair<uint64_t, uint16_t> min = min_heap.top(); min_heap.pop();
	uint16_t min_idx = min.second;

	uint64_t doc_id = get_doc_id(*II_streams[min_idx], stream_idxs[min_idx], prev_doc_ids[min_idx]);
	if (stream_idxs[min_idx] < II_streams[min_idx]->doc_ids.size() - 1) {
		min_heap.push(std::make_pair(doc_id, min_idx));
	}

	return std::make_pair(min.first, min_idx);
}



/*
std::vector<BM25Result> _BM25::_query_partition_streaming(
		std::string& query, 
		uint32_t k,
		uint32_t query_max_df,
		uint16_t partition_id,
		std::vector<float> boost_factors
		) {
	printf("NOT IMPLEMENTED\n");
	return std::vector<BM25Result>();

	std::vector<std::vector<uint64_t>> term_idxs(search_cols.size());
	BM25Partition& IP = index_partitions[partition_id];

	uint64_t doc_offset = (file_type == IN_MEMORY) ? partition_boundaries[partition_id] : 0;

	std::string substr = "";
	for (const char& c : query) {
		if (c != ' ') {
			substr += toupper(c); 
			continue;
		}

		add_query_term(substr, term_idxs, partition_id);	
	}
	if (!substr.empty()) {
		add_query_term(substr, term_idxs, partition_id);
	}

	uint32_t total_terms = 0;
	for (uint16_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		total_terms += term_idxs[col_idx].size();
	}
	if (total_terms == 0) return std::vector<BM25Result>();

	MAP<uint16_t, StandardEntry*> II_streams;
	MAP<uint16_t, BloomEntry*> bloom_entries;

	std::vector<uint64_t> doc_freqs(total_terms, 0);
	std::vector<float> idfs(total_terms, 0);
	uint32_t cntr = 0;
	for (uint16_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		for (const uint64_t& term_idx : term_idxs[col_idx]) {
			doc_freqs[cntr] = IP.II[col_idx].doc_freqs[term_idx];
			idfs[cntr] = log((IP.num_docs - doc_freqs[cntr] + 0.5) / (doc_freqs[cntr] + 0.5));

			auto it = IP.II[col_idx].bloom_filters.find(term_idx);
			if (it != IP.II[col_idx].bloom_filters.end()) {
				bloom_entries[cntr] = &it->second;
				continue;
			}

			II_streams[cntr++] = &IP.II[col_idx].inverted_index_compressed[term_idx];
		}
	}

	std::priority_queue<
		BM25Result,
		std::vector<BM25Result>,
		_compare_bm25_result> top_k_docs;

	// Define minheap of size total_terms to keep track of current smallest doc_id and its index
	std::priority_queue<
		std::pair<uint64_t, uint16_t>,
		std::vector<std::pair<uint64_t, uint16_t>>,
		_compare_64_16> min_heap;

	std::vector<uint64_t> prev_doc_ids(total_terms, 0);
	std::vector<uint32_t> stream_idxs(total_terms, 0);
	std::vector<std::pair<uint16_t, uint32_t>> tf_counters(total_terms, std::make_pair(0, 0));
	std::vector<uint32_t> tf_idxs(total_terms, 0);

	// Initialize min_heap with first doc_id from each term
	for (uint32_t i = 0; i < total_terms; ++i) {
		if (doc_freqs[i] == 0 || doc_freqs[i] > query_max_df) {
			continue;
		}

		uint64_t doc_id = get_doc_id(*II_streams[i], stream_idxs[i], prev_doc_ids[i]);
		min_heap.push(std::make_pair(doc_id, i));

		// Initialize tf_counters
		tf_counters[i].first  = II_streams[i]->term_freqs[0].value;
		tf_counters[i].second = II_streams[i]->term_freqs[0].num_repeats;
		tf_idxs[i] = 1;
	}

	BM25Result current_doc;

	uint64_t global_prev_doc_id = 0;
	while (1) {
		std::pair<uint64_t, uint16_t> doc_id_idx_pair = pop_replace_minheap(
				min_heap, 
				II_streams, 
				stream_idxs, 
				prev_doc_ids
				);

		if (min_heap.size() == 0) {
			break;
		}

		uint64_t doc_id  = doc_id_idx_pair.first;
		uint16_t min_idx = doc_id_idx_pair.second;
		uint16_t col_idx = min_idx / search_cols.size();

		float idf = idfs[min_idx];
		float tf  = (float)tf_counters[min_idx].first;

		// TODO: Add handling for when StandardEntries are empty.

		// Score
		if (doc_id != global_prev_doc_id) {
			// New doc_id.
			// Add previous doc_id to top_k_docs
			if (current_doc.doc_id != 0) {
				// Score with bloom filters
				for (const auto& [i, bloom_entry] : bloom_entries) {
					// TODO: Fix or remove.
					if (bloom_query(bloom_entries[i]->bloom_filters[0], current_doc.doc_id)) {
						current_doc.score += _compute_bm25(
								current_doc.doc_id, 
								1.0f,
								idfs[i], 
								col_idx,
								partition_id
								) * boost_factors[i / search_cols.size()];
					}
				}


				top_k_docs.push(current_doc);
				if (top_k_docs.size() > k) {
					top_k_docs.pop();
				}
			}

			current_doc.doc_id = doc_id + doc_offset;
			current_doc.score = _compute_bm25(
					doc_id, 
					tf, 
					idf, 
					col_idx,
					partition_id
					) * boost_factors[col_idx];
			current_doc.partition_id = partition_id;
		} else {
			// Same doc_id.
			current_doc.score += _compute_bm25(
					doc_id, 
					tf, 
					idf, 
					col_idx,
					partition_id
					) * boost_factors[col_idx];
		}

		--tf_counters[min_idx].second;
		if (tf_counters[min_idx].second == 0) {
			// Get next RLE pair.
			if (tf_idxs[min_idx] < doc_freqs[min_idx]) {
				++tf_idxs[min_idx];
				tf_counters[min_idx].first  = II_streams[min_idx]->term_freqs[tf_idxs[min_idx]].value;
				tf_counters[min_idx].second = II_streams[min_idx]->term_freqs[tf_idxs[min_idx]].num_repeats;
			}
		}

		global_prev_doc_id = doc_id;
	}

	std::vector<BM25Result> result(top_k_docs.size());
	int idx = top_k_docs.size() - 1;
	while (!top_k_docs.empty()) {
		result[idx] = top_k_docs.top();
		top_k_docs.pop();
		--idx;
	}

	return result;
}
*/

std::vector<std::vector<std::pair<std::string, std::string>>> _BM25::get_topk_internal(
		std::string& query,
		uint32_t top_k,
		uint32_t query_max_df,
		std::vector<float> boost_factors
		) {
	std::vector<std::string> copied_query(search_cols.size(), query);
	return get_topk_internal_multi(
			copied_query, 
			top_k, 
			query_max_df, 
			boost_factors
			);
}

std::vector<BM25Result> _BM25::_query_partition_bloom_multi(
		std::vector<std::string>& query,
		uint32_t k,
		uint32_t query_max_df,
		uint16_t partition_id,
		std::vector<float> boost_factors,
		std::vector<std::vector<uint64_t>> doc_freqs
		) {

	std::vector<MAP<uint64_t, BloomEntry>> bloom_entries(search_cols.size());

	BM25PartitionNew* IP = &index_partitions[partition_id];

	uint64_t doc_offset = (file_type == IN_MEMORY) ? partition_boundaries[partition_id] : 0;

	std::vector<std::vector<uint64_t>> term_idxs(search_cols.size());
	std::vector<std::vector<TermType>> term_types(search_cols.size());
	for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		std::string& q = query[col_idx];
		std::string substr = "";

		for (const char& c : q) {
			if (c != ' ') {
				substr += toupper(c);
				continue;
			}

			TermType term_type = add_query_term_bloom(
					substr, 
					term_idxs,
					bloom_entries,
					partition_id,
					col_idx
					);	
			term_types[col_idx].push_back(term_type);
		}

		if (!substr.empty()) {
			TermType term_type = add_query_term_bloom(
					substr, 
					term_idxs,
					bloom_entries,
					partition_id,
					col_idx
					);	
			term_types[col_idx].push_back(term_type);
		}
	}
	assert(term_idxs.size() == doc_freqs.size());

	uint16_t num_low_df_terms  = 0;
	uint16_t num_high_df_terms = 0;
	for (size_t col_idx = 0; col_idx < term_idxs.size(); ++col_idx) {
		assert(term_idxs[col_idx].size() == doc_freqs[col_idx].size());

		for (size_t term_idx = 0; term_idx < term_idxs[col_idx].size(); ++term_idx) {
			TermType term_type = term_types[col_idx][term_idx];
			if (term_type == LOW_DF) {
				++num_low_df_terms;
			} else if (term_type == HIGH_DF) {
				++num_high_df_terms;
			}
		}
	}
	if (num_low_df_terms + num_high_df_terms == 0) return std::vector<BM25Result>();

	// Score low_df terms first.
	MAP<uint64_t, float> doc_scores;

	for (size_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		InvertedIndexNew* II = &IP->II[col_idx];

		for (size_t idx = 0; idx < term_idxs[col_idx].size(); ++idx) {
			if (term_types[col_idx][idx] != LOW_DF) continue;
			uint64_t term_idx = term_idxs[col_idx][idx];

			uint64_t df = doc_freqs[col_idx][idx];
			uint64_t df_partition = IP->II[col_idx].doc_freqs[term_idx];

			if (df == 0 || df > query_max_df || df_partition == 0) continue;

			float idf = log((num_docs - df + 0.5f) / (df + 0.5f));

			for (uint64_t i = 0; i < df_partition; ++i) {

				size_t   idx 	= II->term_offsets[term_idx] + i;
				float    tf 	= (float)II->doc_ids[idx].tf;
				uint64_t doc_id = (uint64_t)II->doc_ids[idx].doc_id;

				// if (doc_id == IP->num_docs) continue;
				assert(doc_id < IP->num_docs);

				float bm25_score = _compute_bm25(
						doc_id, 
						tf, 
						idf, 
						col_idx, 
						partition_id
						) * boost_factors[col_idx];

				doc_id += doc_offset;
				if (doc_scores.find(doc_id) == doc_scores.end()) {
					doc_scores[doc_id] = bm25_score;
				}
				else {
					doc_scores[doc_id] += bm25_score;
				}
			}
		}
	}

	// Now score high_df terms
	if (num_high_df_terms > 0) {
		if (doc_scores.size() == 0) {
			// Sort high_df_term_idxs by df.
			// Get top-k docs for lowest df term. Then use bloom scoring for the rest.
			std::vector<uint32_t> df_values;
			uint32_t min_df = UINT32_MAX;
			uint16_t min_df_col_idx  = UINT16_MAX;
			uint16_t min_df_idx 	 = UINT16_MAX;

			for (uint16_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
				for (uint64_t idx = 0; idx < doc_freqs[col_idx].size(); ++idx) {
					TermType term_type = term_types[col_idx][idx];

					if (term_type != HIGH_DF) continue;

						uint32_t df = doc_freqs[col_idx][idx];
						if (df < min_df) {
							min_df_col_idx = col_idx;
							min_df_idx 	   = idx;
							min_df 		   = df;
						}
						df_values.push_back(df);
				}
			}

			assert(min_df_col_idx  != UINT16_MAX);
			assert(min_df_idx 	   != UINT16_MAX);

			// First score the term with the lowest df.
			float idf = log((num_docs - min_df + 0.5) / (min_df + 0.5));
			BloomEntry& bloom_entry = bloom_entries[min_df_col_idx][min_df_idx];
			for (uint64_t i = 0; i < bloom_entry.topk_doc_ids.size(); ++i) {
				uint64_t doc_id  = bloom_entry.topk_doc_ids[i];
				float tf 		 = (float)bloom_entry.topk_term_freqs[i];
				float bm25_score = _compute_bm25(
						doc_id, 
						tf, 
						idf, 
						min_df_col_idx,
						partition_id
						) * boost_factors[min_df_col_idx];

				doc_id += doc_offset;
				if (doc_scores.find(doc_id) == doc_scores.end()) {
					doc_scores[doc_id] = bm25_score;
				}
				else {
					doc_scores[doc_id] += bm25_score;
				}
			}

			// Now score the rest using bloom filters.
			for (uint16_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
				for (uint16_t idx = 0; idx < term_idxs[col_idx].size(); ++idx) {
					if (term_types[col_idx][idx] != HIGH_DF) continue;
					if (col_idx == min_df_col_idx && idx == min_df_idx) continue;

					const uint64_t& term_idx = term_idxs[col_idx][idx];
					BloomEntry& bloom_entry  = bloom_entries[col_idx][term_idx];

					float df  = doc_freqs[col_idx][idx];
					float idf = log((num_docs - df + 0.5) / (df + 0.5));

					for (auto& [doc_id, score] : doc_scores) {
						for (const auto& [tf, bf] : bloom_entry.bloom_filters) {
							if (bloom_query(bf, doc_id)) {
								score += _compute_bm25(
										doc_id, 
										(float)tf,
										idf, 
										col_idx,
										partition_id
										) * boost_factors[col_idx];
								break;
							}
						}
					}
				}
			}
		} else {

			for (uint16_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
				for (uint16_t idx = 0; idx < term_idxs[col_idx].size(); ++idx) {
					if (term_types[col_idx][idx] != HIGH_DF) continue;

					const uint64_t& term_idx = term_idxs[col_idx][idx];
					BloomEntry& bloom_entry  = bloom_entries[col_idx][term_idx];

					float df  = doc_freqs[col_idx][idx];
					float idf = log((num_docs - df + 0.5) / (df + 0.5));

					for (auto& [doc_id, score] : doc_scores) {
						for (const auto& [tf, bf] : bloom_entry.bloom_filters) {
							if (bloom_query(bf, doc_id)) {
								score += _compute_bm25(
										doc_id, 
										(float)tf,
										idf, 
										col_idx,
										partition_id
										) * boost_factors[col_idx];
								break;
							}
						}
					}
				}
			}
		}
	}

	if (doc_scores.size() == 0) {
		return std::vector<BM25Result>();
	}

	std::priority_queue<
		BM25Result,
		std::vector<BM25Result>,
		_compare_bm25_result> top_k_docs;

	for (const auto& pair : doc_scores) {
		BM25Result result;
		result.doc_id = pair.first;
		result.score  = pair.second;
		result.partition_id = partition_id;

		if (top_k_docs.size() < k) {
			top_k_docs.push(result);
		} else {
			if (result.score > top_k_docs.top().score) {
				top_k_docs.pop();
				top_k_docs.push(result);
			}
		}
	}

	std::vector<BM25Result> result(top_k_docs.size());
	int idx = top_k_docs.size() - 1;
	while (!top_k_docs.empty()) {
		result[idx] = top_k_docs.top();
		top_k_docs.pop();
		--idx;
	}

	return result;
}

std::vector<BM25Result> _BM25::query(
		std::string& query,
		uint32_t top_k,
		uint32_t query_max_df,
		std::vector<float> boost_factors
		) {
	std::vector<std::string> copied_query(search_cols.size(), query);
	return query_multi(
			copied_query, 
			top_k, 
			query_max_df, 
			boost_factors
			);
}


std::vector<BM25Result> _BM25::query_multi(
		std::vector<std::string>& query,
		uint32_t k,
		uint32_t query_max_df,
		std::vector<float> boost_factors
		) {
	auto start = std::chrono::high_resolution_clock::now();

	std::vector<std::vector<uint64_t>> doc_freqs(search_cols.size());
	for (uint16_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
		std::string& q = query[col_idx];
		std::string substr = "";

		for (const char& c : q) {
			if (c != ' ') {
				substr += toupper(c);
				continue;
			} else {
				uint64_t df_sum = get_doc_freqs_sum(substr, col_idx);
				doc_freqs[col_idx].push_back(df_sum);
				substr.clear();
			}
		}

		if (!substr.empty()) {
			uint64_t df_sum = get_doc_freqs_sum(substr, col_idx);
			doc_freqs[col_idx].push_back(df_sum);
		}
	}

	if (boost_factors.size() == 0) {
		boost_factors.resize(search_cols.size());
		memset(
				&boost_factors[0],
				1.0f,
				search_cols.size() * sizeof(float)
			  );
	}

	if (boost_factors.size() != search_cols.size()) {
		std::cout << "Error: Boost factors must be the same size as the number of search fields." << std::endl;
		std::cout << "Number of search fields: " << search_cols.size() << std::endl;
		std::cout << "Number of boost factors: " << boost_factors.size() << std::endl;
		std::exit(1);
	}

	std::vector<std::thread> threads;
	std::vector<std::vector<BM25Result>> results(num_partitions);

	// _query_partition on each thread
	for (uint16_t i = 0; i < num_partitions; ++i) {
		threads.push_back(std::thread(
			[this, &query, k, query_max_df, i, &results, boost_factors, doc_freqs] {
				results[i] = _query_partition_bloom_multi(
						query, 
						k, 
						query_max_df, 
						i, 
						boost_factors,
						doc_freqs
						);
			}
		));
	}

	for (auto& thread : threads) {
		thread.join();
	}

	if (results.size() == 0) {
		return std::vector<BM25Result>();
	}

	uint64_t total_matching_docs = 0;

	// Join results. Keep global max heap of size k
	std::priority_queue<
		BM25Result,
		std::vector<BM25Result>,
		_compare_bm25_result> top_k_docs;

	for (const auto& partition_results : results) {
		total_matching_docs += partition_results.size();
		for (const auto& pair : partition_results) {
			top_k_docs.push(pair);
			if (top_k_docs.size() > k) {
				top_k_docs.pop();
			}
		}
	}
	
	std::vector<BM25Result> result(top_k_docs.size());
	int idx = top_k_docs.size() - 1;
	while (!top_k_docs.empty()) {
		result[idx] = top_k_docs.top();
		top_k_docs.pop();
		--idx;
	}

	if (DEBUG) {
		auto end = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double, std::milli> elapsed_ms = end - start;
		for (const auto& q : query) {
			std::cout << "QUERY: " << q;
		}
		std::cout << std::endl;
		std::cout << "Total matching docs: " << total_matching_docs << "    ";
		std::cout << elapsed_ms.count() << "ms" << std::endl;
		fflush(stdout);
	}

	return result;
}


std::vector<std::vector<std::pair<std::string, std::string>>> _BM25::get_topk_internal_multi(
		std::vector<std::string>& _query,
		uint32_t top_k,
		uint32_t query_max_df,
		std::vector<float> boost_factors
		) {

	std::vector<std::vector<std::pair<std::string, std::string>>> result;
	std::vector<BM25Result> top_k_docs = query_multi(_query, top_k, query_max_df, boost_factors);
	result.reserve(top_k_docs.size());

	std::vector<std::pair<std::string, std::string>> row;
	for (size_t i = 0; i < top_k_docs.size(); ++i) {
		switch (file_type) {
			case CSV:
				row = get_csv_line(top_k_docs[i].doc_id, top_k_docs[i].partition_id);
				break;
			case JSON:
				row = get_json_line(top_k_docs[i].doc_id, top_k_docs[i].partition_id);
				break;
			case IN_MEMORY:
				std::cout << "Error: In-memory data not supported for this function." << std::endl;
				std::exit(1);
				break;
			default:
				std::cout << "Error: Incorrect file type" << std::endl;
				std::exit(1);
				break;
		}
		row.push_back(std::make_pair("score", std::to_string(top_k_docs[i].score)));
		result.push_back(row);
	}
	return result;
}
