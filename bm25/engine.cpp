#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>

#include <iostream>
#include <vector>
#include <algorithm>
#include <queue>
#include <string>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <sstream>

#include <chrono>
#include <ctime>
#include <sys/mman.h>
#include <fcntl.h>
#include <omp.h>
#include <thread>
#include <mutex>
#include <termios.h>

#include "engine.h"
#include "robin_hood.h"
#include "vbyte_encoding.h"
#include "serialize.h"


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


void _BM25::determine_partition_boundaries_csv() {
	// First find number of bytes in file.
	// Get avg chunk size in bytes.
	// Seek in jumps of byte chunks, then scan forward to newline and append to partition_boundaries.
	// If we reach end of file, break.

	FILE* f = reference_file_handles[0];

	struct stat sb;
	if (fstat(fileno(f), &sb) == -1) {
		std::cerr << "Error getting file size." << std::endl;
		std::exit(1);
	}

	size_t file_size = sb.st_size;
	size_t chunk_size = file_size / num_partitions;

	if (max_df < 2.0f) {
		// Guess for now
		this->max_df = (int)file_size * max_df / 100;
	}

	partition_boundaries.push_back(header_bytes);

	size_t byte_offset = header_bytes;
	while (true) {
		byte_offset += chunk_size;

		if (byte_offset >= file_size) {
			partition_boundaries.push_back(file_size);
			break;
		}

		fseek(f, byte_offset, SEEK_SET);

		char buf[1024];
		while (true) {
			size_t bytes_read = fread(buf, 1, sizeof(buf), f);
			for (size_t i = 0; i < bytes_read; ++i) {
				if (buf[i] == '\n') {
					partition_boundaries.push_back(++byte_offset);
					goto end_of_loop;
				}
				++byte_offset;
			}
		}

		end_of_loop:
			continue;
	}

	if (partition_boundaries.size() != num_partitions + 1) {
		printf("Partition boundaries: %lu\n", partition_boundaries.size());
		printf("Num partitions: %d\n", num_partitions);
		std::cerr << "Error determining partition boundaries." << std::endl;
		std::exit(1);
	}

	// Reset file pointer to beginning
	fseek(f, header_bytes, SEEK_SET);
}

void _BM25::determine_partition_boundaries_json() {
	// Same as csv for now. Assuming newline delimited json.
	determine_partition_boundaries_csv();
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
		columns.push_back(value);
		for (const auto& col : search_cols) {
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
			++(rle_row.back().num_repeats);
		}
		else {
			RLEElement_u8 rle = init_rle_element_u8(value);
			rle_row.push_back(rle);
		}
	}
}

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

	uint64_t char_idx = 0;

	std::string term = "";

	robin_hood::unordered_flat_map<uint64_t, uint8_t> terms_seen;
	printf("Doc id: %lu\n", doc_id);
	fflush(stdout);

	// Split by commas not inside double quotes
	uint64_t doc_size = 0;
	while (doc[char_idx] != terminator) {
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

		if (doc[char_idx] == ' ' && term == "") {
			++char_idx;
			continue;
		}

		if (doc[char_idx] == ' ') {
			if (stop_words.find(term) != stop_words.end()) {
				term.clear();
				++char_idx;
				++doc_size;
				continue;
			}

			auto [it, add] = IP.unique_term_mapping[col_idx].try_emplace(term, unique_terms_found);
			if (add) {
				// New term
				terms_seen.insert({it->second, 1});
				II.inverted_index_compressed.push_back(InvertedIndexElement());
				II.prev_doc_ids.push_back(0);
				++unique_terms_found;
			}
			else {
				// Term already exists

				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
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
		if (stop_words.find(term) == stop_words.end()) {
			auto [it, add] = IP.unique_term_mapping[col_idx].try_emplace(term, unique_terms_found);

			if (add) {
				// New term
				terms_seen.insert({it->second, 1});
				II.inverted_index_compressed.push_back(InvertedIndexElement());
				II.prev_doc_ids.push_back(0);
				++unique_terms_found;
			}
			else {
				// Term already exists

				if (terms_seen.find(it->second) == terms_seen.end()) {
					terms_seen.insert({it->second, 1});
				}
				else {
					++(terms_seen[it->second]);
				}
			}
		}
		++doc_size;
	}

	if (doc_id == IP.doc_sizes.size() - 1) {
		IP.doc_sizes[doc_id] += (uint16_t)doc_size;
	}
	else {
		IP.doc_sizes.push_back((uint16_t)doc_size);
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

        progress_bars.resize(std::max(progress_bars.size(), static_cast<size_t>(partition_id + 1)));
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


IIRow get_II_row(
		InvertedIndex* II, 
		uint64_t term_idx,
		uint32_t k
		) {
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
					(float)II->inverted_index_compressed[term_idx].term_freqs[i].value
					);
		}
	}

	return row;
}

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
		if (line[char_idx] == '}') return;

		if (line[char_idx] == '\\') {
			char_idx += 2;
			continue;
		}

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

void _BM25::read_json(uint64_t start_byte, uint64_t end_byte, uint16_t partition_id) {
	FILE* f = reference_file_handles[partition_id];
	BM25Partition& IP = index_partitions[partition_id];

	// Quickly count number of lines in file
	uint64_t num_lines = 0;
	uint64_t total_bytes_read = 0;
	char buf[1024 * 64];

	if (fseek(f, start_byte, SEEK_SET) != 0) {
		std::cerr << "Error seeking file." << std::endl;
		std::exit(1);
	}

	while (total_bytes_read < (end_byte - start_byte)) {
		size_t bytes_read = fread(buf, 1, sizeof(buf), f);
		if (bytes_read == 0) {
			break;
		}

		for (size_t i = 0; i < bytes_read; ++i) {
			if (buf[i] == '\n') {
				++num_lines;
			}
			if (++total_bytes_read >= (end_byte - start_byte)) {
				break;
			}
		}
	}

	IP.num_docs = num_lines;

	// Reset file pointer to beginning
	fseek(f, start_byte, SEEK_SET);

	// Read the file line by line
	char*    line = NULL;
	size_t   len = 0;
	ssize_t  read;
	uint64_t line_num = 0;
	uint64_t byte_offset = start_byte;

	uint32_t unique_terms_found = 0;

	const int UPDATE_INTERVAL = 10000;
	while ((read = getline(&line, &len, f)) != -1) {

		if (byte_offset >= end_byte) {
			break;
		}

		if (!DEBUG) {
			if (line_num % UPDATE_INTERVAL == 0) {
				update_progress(line_num, num_lines, partition_id);
			}
		}
		if (strlen(line) == 0) {
			std::cout << "Empty line found" << std::endl;
			std::exit(1);
		}

		IP.line_offsets.push_back(byte_offset);
		byte_offset += read;

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
								scan_to_next_key(line, char_idx);
								key.clear();
								++search_col_idx;
								goto start;
							}

							// Iter over quote.
							++char_idx;

							char_idx += process_doc_partition(
									&line[char_idx], 
									'"', 
									line_num, 
									unique_terms_found, 
									partition_id,
									search_col_idx++
									); ++char_idx;
							scan_to_next_key(line, char_idx);
							key.clear();
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
				else if (char_idx > 1048576) {
					std::cout << "Search field not found on line: " << line_num << std::endl;
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

		if (!DEBUG) {
			if (line_num % UPDATE_INTERVAL == 0) update_progress(line_num, num_lines, partition_id);
		}

		++line_num;
	}

	if (!DEBUG) update_progress(line_num, num_lines, partition_id);

	free(line);

	if (DEBUG) {
		std::cout << "Vocab size: " << unique_terms_found << std::endl;
	}

	IP.num_docs = num_lines;

	// Calc avg_doc_size
	double avg_doc_size = 0;
	for (const auto& size : IP.doc_sizes) {
		avg_doc_size += (double)size;
	}
	IP.avg_doc_size = (float)(avg_doc_size / num_lines);

	for (uint16_t search_col_idx = 0; search_col_idx < IP.II.size(); ++search_col_idx) {
		IP.II[search_col_idx].prev_doc_ids.clear();
		IP.II[search_col_idx].prev_doc_ids.shrink_to_fit();

		for (auto& row : IP.II[search_col_idx].inverted_index_compressed) {
			if (row.doc_ids.size() == 0 || get_rle_u8_row_size(row.term_freqs) < (uint64_t)min_df) {
				row.doc_ids.clear();
				row.doc_ids.clear();
				row.term_freqs.clear();
				row.term_freqs.shrink_to_fit();
			}
		}
	}
}

void _BM25::read_csv(uint64_t start_byte, uint64_t end_byte, uint16_t partition_id) {
	FILE* f = reference_file_handles[partition_id];
	BM25Partition& IP = index_partitions[partition_id];

	// Quickly count number of lines in file
	uint64_t num_lines = 0;
	uint64_t total_bytes_read = 0;
	char buf[1024 * 64];

	if (fseek(f, start_byte, SEEK_SET) != 0) {
		std::cerr << "Error seeking file." << std::endl;
		std::exit(1);
	}

	while (total_bytes_read < (end_byte - start_byte)) {
		size_t bytes_read = fread(buf, 1, sizeof(buf), f);
		if (bytes_read == 0) {
			break;
		}

		for (size_t i = 0; i < bytes_read; ++i) {
			if (buf[i] == '\n') {
				++num_lines;
			}
			if (++total_bytes_read >= (end_byte - start_byte)) {
				break;
			}
		}
	}

	IP.num_docs = num_lines;

	// Reset file pointer to beginning
	if (fseek(f, start_byte, SEEK_SET) != 0) {
		std::cerr << "Error seeking file." << std::endl;
		std::exit(1);
	}

		// Read the file line by line
	char*    line = NULL;
	size_t   len = 0;
	ssize_t  read;
	uint64_t line_num = 0;
	uint64_t byte_offset = start_byte;

	uint32_t unique_terms_found = 0;

	// Small string optimization limit on most platforms
	std::string doc = "";
	doc.reserve(22);

	char end_delim = ',';

	const int UPDATE_INTERVAL = 10000;
	while ((read = getline(&line, &len, f)) != -1) {

		if (byte_offset >= end_byte) {
			break;
		}

		if (!DEBUG) {
			if (line_num % UPDATE_INTERVAL == 0) update_progress(line_num, num_lines, partition_id);
		}

		IP.line_offsets.push_back(byte_offset);
		byte_offset += read;

		int char_idx = 0;
		int col_idx  = 0;
		uint16_t _search_col_idx = 0;
		for (const auto& search_col_idx : search_col_idxs) {

			if (search_col_idx == (int)columns.size() - 1) {
				end_delim = '\n';
			}
			else {
				end_delim = ',';
			}

			// Iterate of line chars until we get to relevant column.
			while (col_idx != search_col_idx) {
				if (line[char_idx] == '\\') {
					char_idx += 2;
					continue;
				}

				if (line[char_idx] == '"') {
					// Skip to next unescaped quote
					++char_idx;

					while (line[char_idx] != '"') {
						if (line[char_idx] == '\\') {
							char_idx += 2;
							continue;
						}
						++char_idx;
					}
					++char_idx;
				}

				if (line[char_idx] == ',') ++col_idx;
				++char_idx;
			}
			++col_idx;

			// Split by commas not inside double quotes
			if (line[char_idx] == '"') {
				++char_idx;
				char_idx += process_doc_partition(
					&line[char_idx], 
					'"', 
					line_num, 
					unique_terms_found, 
					partition_id,
					_search_col_idx++
					); ++char_idx;
				continue;
			}

			char_idx += process_doc_partition(
				&line[char_idx], 
				end_delim,
				line_num, 
				unique_terms_found, 
				partition_id,
				_search_col_idx++
				); ++char_idx;
		}
		++line_num;
	}
	if (!DEBUG) update_progress(line_num + 1, num_lines, partition_id);

	if (DEBUG) {
		std::cout << "Vocab size: " << unique_terms_found << std::endl;
	}


	IP.num_docs = IP.doc_sizes.size();

	// Calc avg_doc_size
	double avg_doc_size = 0;
	for (const auto& size : IP.doc_sizes) {
		avg_doc_size += (double)size;
	}
	IP.avg_doc_size = (float)(avg_doc_size / num_lines);

	for (uint16_t col_idx = 0; col_idx < search_col_idxs.size(); ++col_idx) {
		IP.II[col_idx].prev_doc_ids.clear();
		IP.II[col_idx].prev_doc_ids.shrink_to_fit();

		for (auto& row : IP.II[col_idx].inverted_index_compressed) {
			if (row.doc_ids.size() == 0 || get_rle_u8_row_size(row.term_freqs) < (uint64_t)min_df) {
				row.doc_ids.clear();
				row.doc_ids.clear();
				row.term_freqs.clear();
				row.term_freqs.shrink_to_fit();
			}
		}
	}
}

void _BM25::read_in_memory(
		std::vector<std::string>& documents,
		uint64_t start_idx, 
		uint64_t end_idx, 
		uint16_t partition_id
		) {
	// TODO: Fix

	BM25Partition& IP = index_partitions[partition_id];

	IP.num_docs = end_idx - start_idx;

	uint32_t unique_terms_found = 0;

	uint32_t cntr = 0;
	const int UPDATE_INTERVAL = 10000;

	for (uint64_t line_num = start_idx; line_num < end_idx; ++line_num) {
		std::string& doc = documents[line_num];

		if (!DEBUG) {
			if (cntr % UPDATE_INTERVAL == 0) {
				update_progress(cntr, IP.num_docs, partition_id);
			}
		}

		process_doc_partition(
			(doc + "\n").c_str(),
			'\n',
			cntr, 
			unique_terms_found, 
			partition_id,
			0
			);

		++cntr;
	}
	if (!DEBUG) update_progress(cntr + 1, IP.num_docs, partition_id);


	// Calc avg_doc_size
	double avg_doc_size = 0;
	for (const auto& size : IP.doc_sizes) {
		avg_doc_size += (double)size;
	}
	IP.avg_doc_size = (float)(avg_doc_size / IP.num_docs);

	for (uint16_t col_idx = 0; col_idx < search_col_idxs.size(); ++col_idx) {
		IP.II[col_idx].prev_doc_ids.clear();
		IP.II[col_idx].prev_doc_ids.shrink_to_fit();

		for (auto& row : IP.II[col_idx].inverted_index_compressed) {
			if (row.doc_ids.size() == 0 || get_rle_u8_row_size(row.term_freqs) < (uint64_t)min_df) {
				row.doc_ids.clear();
				row.doc_ids.clear();
				row.term_freqs.clear();
				row.term_freqs.shrink_to_fit();
			}
		}
	}
}


std::vector<std::pair<std::string, std::string>> _BM25::get_csv_line(int line_num, uint16_t partition_id) {
	FILE* f = reference_file_handles[partition_id];
	BM25Partition& IP = index_partitions[partition_id];

	// seek from FILE* reference_file which is already open
	fseek(f, IP.line_offsets[line_num], SEEK_SET);
	char* line = NULL;
	size_t len = 0;
	ssize_t read = getline(&line, &len, f);

	// Create effective json by combining column names with values split by commas
	std::vector<std::pair<std::string, std::string>> row;
	std::string cell;
	bool in_quotes = false;
	size_t col_idx = 0;

	for (size_t i = 0; i < (size_t)read - 1; ++i) {
		if (line[i] == '"') {
			in_quotes = !in_quotes;
		}
		else if (line[i] == ',' && !in_quotes) {
			row.emplace_back(columns[col_idx], cell);
			cell.clear();
			++col_idx;
		}
		else {
			cell += line[i];
		}
	}
	row.emplace_back(columns[col_idx], cell);
	return row;
}


std::vector<std::pair<std::string, std::string>> _BM25::get_json_line(int line_num, uint16_t partition_id) {
	FILE* f = reference_file_handles[partition_id];
	BM25Partition& IP = index_partitions[partition_id];

	// seek from FILE* reference_file which is already open
	fseek(f, IP.line_offsets[line_num], SEEK_SET);
	char* line = NULL;
	size_t len = 0;
	getline(&line, &len, f);

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
	serialize_vector_u16(IP.doc_sizes, DOC_SIZES_PATH + "_" + std::to_string(partition_id));

	std::vector<uint8_t> compressed_line_offsets;
	compressed_line_offsets.reserve(IP.line_offsets.size() * 2);
	compress_uint64(IP.line_offsets, compressed_line_offsets);

	serialize_vector_u8(compressed_line_offsets, LINE_OFFSETS_PATH + "_" + std::to_string(partition_id));
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
	deserialize_vector_u16(IP.doc_sizes, DOC_SIZES_PATH + "_" + std::to_string(partition_id));

	std::vector<uint8_t> compressed_line_offsets;
	deserialize_vector_u8(compressed_line_offsets, LINE_OFFSETS_PATH + "_" + std::to_string(partition_id));

	decompress_uint64(compressed_line_offsets, IP.line_offsets);
}

void _BM25::save_to_disk(const std::string& db_dir) {
	auto start = std::chrono::high_resolution_clock::now();

	if (access(db_dir.c_str(), F_OK) != -1) {
		// Remove the directory if it exists
		std::string command = "rm -r " + db_dir;
		system(command.c_str());

		// Create the directory
		command = "mkdir " + db_dir;
		system(command.c_str());
	}
	else {
		// Create the directory if it does not exist
		std::string command = "mkdir " + db_dir;
		system(command.c_str());
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

	// Serialize smaller members.
	std::ofstream out_file(METADATA_PATH, std::ios::binary);
	if (!out_file) {
		std::cerr << "Error opening file for writing.\n";
		return;
	}

	// Write basic types directly
	out_file.write(reinterpret_cast<const char*>(&num_docs), sizeof(num_docs));
	out_file.write(reinterpret_cast<const char*>(&min_df), sizeof(min_df));
	out_file.write(reinterpret_cast<const char*>(&max_df), sizeof(max_df));
	out_file.write(reinterpret_cast<const char*>(&k1), sizeof(k1));
	out_file.write(reinterpret_cast<const char*>(&b), sizeof(b));
	out_file.write(reinterpret_cast<const char*>(&num_partitions), sizeof(num_partitions));

	for (uint16_t partition_id = 0; partition_id < num_partitions; ++partition_id) {
		BM25Partition& IP = index_partitions[partition_id];

		out_file.write(reinterpret_cast<const char*>(&IP.avg_doc_size), sizeof(IP.avg_doc_size));
	}

	// Write enum as int
	int file_type_int = static_cast<int>(file_type);
	out_file.write(reinterpret_cast<const char*>(&file_type_int), sizeof(file_type_int));

	// Write std::string
	size_t filename_length = filename.size();
	out_file.write(reinterpret_cast<const char*>(&filename_length), sizeof(filename_length));
	out_file.write(filename.data(), filename_length);

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
	std::string DOC_SIZES_PATH 		     = db_dir + "/doc_sizes.bin";
	std::string LINE_OFFSETS_PATH 		 = db_dir + "/line_offsets.bin";
	std::string METADATA_PATH 			 = db_dir + "/metadata.bin";

	// Load smaller members.
	std::ifstream in_file(METADATA_PATH, std::ios::binary);
    if (!in_file) {
        std::cerr << "Error opening file for reading.\n";
        return;
    }

    // Read basic types directly
    in_file.read(reinterpret_cast<char*>(&num_docs), sizeof(num_docs));
    in_file.read(reinterpret_cast<char*>(&min_df), sizeof(min_df));
    in_file.read(reinterpret_cast<char*>(&max_df), sizeof(max_df));
    in_file.read(reinterpret_cast<char*>(&k1), sizeof(k1));
    in_file.read(reinterpret_cast<char*>(&b), sizeof(b));
	in_file.read(reinterpret_cast<char*>(&num_partitions), sizeof(num_partitions));

	index_partitions.clear();
	index_partitions.resize(num_partitions);

	std::vector<std::thread> threads;

	for (uint16_t partition_id = 0; partition_id < num_partitions; ++partition_id) {
		threads.push_back(std::thread(&_BM25::load_index_partition, this, db_dir, partition_id));
	}

	for (auto& thread : threads) {
		thread.join();
	}

	// Load rest of metadata.
	for (uint16_t partition_id = 0; partition_id < num_partitions; ++partition_id) {
		BM25Partition& IP = index_partitions[partition_id];

		in_file.read(reinterpret_cast<char*>(&IP.avg_doc_size), sizeof(IP.avg_doc_size));
	}

    // Read enum as int
    int file_type_int;
    in_file.read(reinterpret_cast<char*>(&file_type_int), sizeof(file_type_int));
    file_type = static_cast<SupportedFileTypes>(file_type_int);

    // Read std::string
    size_t filename_length;
    in_file.read(reinterpret_cast<char*>(&filename_length), sizeof(filename_length));
    filename.resize(filename_length);
    in_file.read(&filename[0], filename_length);

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

    in_file.close();

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed_seconds = end - start;

	if (DEBUG) {
		std::cout << "Loaded in " << elapsed_seconds.count() << "s" << std::endl;
	}
}


_BM25::_BM25(
		std::string filename,
		std::vector<std::string> search_cols,
		int min_df,
		float max_df,
		float k1,
		float b,
		uint16_t num_partitions,
		const std::vector<std::string>& _stop_words
		) : min_df(min_df), 
			max_df(max_df), 
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

	index_partitions.resize(num_partitions);
	for (uint16_t i = 0; i < num_partitions; ++i) {
		index_partitions[i].II.resize(search_cols.size());
		index_partitions[i].unique_term_mapping.resize(search_cols.size());
	}

	num_docs = 0;

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

	// Read file to get documents, line offsets, and columns
	if (filename.substr(filename.size() - 3, 3) == "csv") {

		proccess_csv_header();
		determine_partition_boundaries_csv();

		// Launch num_partitions threads to read csv file
		for (uint16_t i = 0; i < num_partitions; ++i) {
			threads.push_back(std::thread(
				[this, i] {
					read_csv(partition_boundaries[i], partition_boundaries[i + 1], i);
				}
			));
		}

		file_type = CSV;
	}
	else if (filename.substr(filename.size() - 4, 4) == "json") {
		header_bytes = 0;
		determine_partition_boundaries_json();

		// Launch num_partitions threads to read json file
		for (uint16_t i = 0; i < num_partitions; ++i) {
			threads.push_back(std::thread(
				[this, i] {
					read_json(partition_boundaries[i], partition_boundaries[i + 1], i);
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
	for (uint16_t i = 0; i < num_partitions; ++i) {
		num_docs += index_partitions[i].num_docs;
	}

	if (!DEBUG) finalize_progress_bar();

	uint64_t total_size = 0;
	uint32_t unique_terms_found = 0;
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
	uint64_t vocab_size = unique_terms_found * (4 + 5 + 1) / 1048576;
	uint64_t line_offsets_size = num_docs * 8 / 1048576;
	uint64_t doc_sizes_size = num_docs * 2 / 1048576;
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

	if (DEBUG) {
		auto read_end = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double> read_elapsed_seconds = read_end - overall_start;
		std::cout << "Read file in " << read_elapsed_seconds.count() << " seconds" << std::endl;
	}
}


_BM25::_BM25(
		std::vector<std::string>& documents,
		int min_df,
		float max_df,
		float k1,
		float b,
		uint16_t num_partitions,
		const std::vector<std::string>& _stop_words
		) : min_df(min_df), 
			max_df(max_df), 
			k1(k1), 
			b(b),
			num_partitions(num_partitions) {
	
	for (const std::string& stop_word : _stop_words) {
		stop_words.insert(stop_word);
	}

	filename = "in_memory";
	file_type = IN_MEMORY;

	num_docs = documents.size();

	if (max_df < 2.0f) {
		this->max_df = (int)num_docs * max_df;
	}

	index_partitions.resize(num_partitions);
	partition_boundaries.resize(num_partitions + 1);

	for (uint16_t i = 0; i < num_partitions; ++i) {
		partition_boundaries[i] = (uint64_t)i * (num_docs / num_partitions);
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
			}
		));
	}

	for (auto& thread : threads) {
		thread.join();
	}

	if (!DEBUG) finalize_progress_bar();

	uint64_t total_size = 0;
	uint32_t unique_terms_found = 0;
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
	uint64_t vocab_size = unique_terms_found * (4 + 5 + 1) / 1048576;
	uint64_t line_offsets_size = num_docs * 8 / 1048576;
	uint64_t doc_sizes_size = num_docs * 2 / 1048576;
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

	for (uint16_t i = 0; i < num_partitions; ++i) {
		BM25Partition& IP = index_partitions[i];

		for (uint16_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
			IP.II[col_idx].prev_doc_ids.clear();
			IP.II[col_idx].prev_doc_ids.shrink_to_fit();

			for (auto& row : IP.II[col_idx].inverted_index_compressed) {
				if (row.doc_ids.size() == 0 || get_rle_u8_row_size(row.term_freqs) < (uint64_t)min_df) {
					row.doc_ids.clear();
					row.doc_ids.clear();
					row.term_freqs.clear();
					row.term_freqs.shrink_to_fit();
				}
			}
		}
	}
}

inline float _BM25::_compute_bm25(
		uint64_t doc_id,
		float tf,
		float idf,
		uint16_t partition_id
		) {
	BM25Partition& IP = index_partitions[partition_id];

	float doc_size = IP.doc_sizes[doc_id];
	return idf * tf / (tf + k1 * (1 - b + b * doc_size / IP.avg_doc_size));
}

std::vector<BM25Result> _BM25::_query_partition(
		std::string& query, 
		uint32_t k,
		uint32_t query_max_df,
		uint16_t partition_id,
		std::vector<float> boost_factors
		) {
	auto start = std::chrono::high_resolution_clock::now();
	std::vector<uint64_t> term_idxs;
	BM25Partition& IP = index_partitions[partition_id];

	uint64_t doc_offset = (file_type == IN_MEMORY) ? partition_boundaries[partition_id] : 0;

	std::string substr = "";
	for (const char& c : query) {
		if (c != ' ') {
			substr += toupper(c); 
			continue;
		}

		for (uint16_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
			robin_hood::unordered_map<std::string, uint32_t>& vocab = IP.unique_term_mapping[col_idx];

			if (vocab.find(substr) == vocab.end()) {
				continue;
			}

			term_idxs.push_back(vocab[substr]);
			substr.clear();
		}

	}

	if (term_idxs.size() == 0) {
		return std::vector<BM25Result>();
	}

	// Gather docs that contain at least one term from the query
	// Uses dynamic max_df for performance
	robin_hood::unordered_map<uint64_t, float> doc_scores;

	double total_get_row_time = 0;
	for (const uint64_t& term_idx : term_idxs) {
		for (uint16_t col_idx = 0; col_idx < search_cols.size(); ++col_idx) {
			float boost_factor = boost_factors[col_idx];

			uint64_t df = get_rle_u8_row_size(IP.II[col_idx].inverted_index_compressed[term_idx].term_freqs);

			if (df == 0) {
				continue;
			}

			if (df > query_max_df) continue;

			float idf = log((IP.num_docs - df + 0.5) / (df + 0.5));

			auto get_row_start = std::chrono::high_resolution_clock::now();
			IIRow row = get_II_row(&IP.II[col_idx], term_idx, k);
			auto get_row_end = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double, std::milli> get_row_elapsed_ms = get_row_end - get_row_start;
			total_get_row_time += get_row_elapsed_ms.count();

			// Partial sort row.doc_ids by row.term_freqs to get top k
			for (uint64_t i = 0; i < df; ++i) {

				uint64_t doc_id  = row.doc_ids[i];
				float tf 		 = row.term_freqs[i];
				float bm25_score = _compute_bm25(doc_id, tf, idf, partition_id) * boost_factor;

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

	if (DEBUG) {
		auto end = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double, std::milli> elapsed_ms = end - start;
		std::cout << "Number of docs: " << doc_scores.size() << "   GATHER TIME: ";
		std::cout << elapsed_ms.count() << "ms" << std::endl;

		std::cout << "GET ROW TIME: " << total_get_row_time << "ms" << std::endl;
		fflush(stdout);
	}
	
	if (doc_scores.size() == 0) {
		return std::vector<BM25Result>();
	}

	start = std::chrono::high_resolution_clock::now();
	std::priority_queue<
		BM25Result,
		std::vector<BM25Result>,
		_compare_bm25_result> top_k_docs;

	for (const auto& pair : doc_scores) {
		BM25Result result {
			.doc_id = pair.first,
			.score  = pair.second,
			.partition_id = partition_id
		};
		top_k_docs.push(result);
		if (top_k_docs.size() > k) {
			top_k_docs.pop();
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
		std::cout << "QUERY: " << query << std::endl;
		std::cout << "Number of docs: " << doc_scores.size() << "    ";
		std::cout << elapsed_ms.count() << "ms" << std::endl;
		fflush(stdout);
	}

	return result;
}

std::vector<BM25Result> _BM25::query(
		std::string& query, 
		uint32_t k,
		uint32_t query_max_df,
		std::vector<float> boost_factors
		) {
	auto start = std::chrono::high_resolution_clock::now();

	std::vector<std::thread> threads;
	std::vector<std::vector<BM25Result>> results(num_partitions);

	// _query_partition on each thread
	for (uint16_t i = 0; i < num_partitions; ++i) {
		threads.push_back(std::thread(
			[this, &query, k, query_max_df, i, &results, boost_factors] {
				results[i] = _query_partition(query, k, query_max_df, i, boost_factors);
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
		std::cout << "QUERY: " << query << std::endl;
		std::cout << "Total matching docs: " << total_matching_docs << "    ";
		std::cout << elapsed_ms.count() << "ms" << std::endl;
		fflush(stdout);
	}

	return result;
}

std::vector<std::vector<std::pair<std::string, std::string>>> _BM25::get_topk_internal(
		std::string& _query,
		uint32_t top_k,
		uint32_t query_max_df,
		std::vector<float> boost_factors
		) {

	if (boost_factors.size() != search_cols.size()) {
		std::cout << "Error: Boost factors must be the same size as the number of search fields." << std::endl;
		std::cout << "Number of search fields: " << search_cols.size() << std::endl;
		std::cout << "Number of boost factors: " << boost_factors.size() << std::endl;
		std::exit(1);
	}

	std::vector<std::vector<std::pair<std::string, std::string>>> result;
	std::vector<BM25Result> top_k_docs = query(_query, top_k, query_max_df, boost_factors);
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
