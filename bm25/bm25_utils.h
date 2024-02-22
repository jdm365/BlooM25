#include <vector>
#include <string>
#include <cstdint>
#include <bitset>

#include "robin_hood.h"
#include <leveldb/db.h>

static const std::string DIR_NAME      			= "bm25_db";
static const std::string DOC_TERM_FREQS_DB_NAME = "DOC_TERM_FREQS";
static const std::string INVERTED_INDEX_DB_NAME = "INVERTED_INDEX";
static const std::string TERM_FREQS_FILE_NAME   = "TERM_FREQS";

#define INIT_MAX_DF 500
#define DEBUG 0

std::vector<std::string> tokenize_whitespace(
		const std::string& document
		);
void tokenize_whitespace_batch(
		std::vector<std::string>& documents,
		std::vector<std::vector<std::string>>& tokenized_documents
		);

struct SmallStringSet {
	std::bitset<16> bitset;
	std::vector<std::string> vec;

	void insert(const std::string& str) {
		if (bitset.size() < 16) {
			if (!bitset.test(str.size())) {
				bitset.set(str.size());
				vec.push_back(str);
			}
		} else {
			vec.push_back(str);
		}
	}

	// Define iterator to get all elements with (auto x : set)
	typedef std::vector<std::string>::iterator iterator;
	iterator begin() { return vec.begin(); }
	iterator end() { return vec.end(); }
};

class _BM25 {
	public:
		std::vector<std::vector<std::pair<std::string, uint16_t>>> term_freqs;
		std::vector<uint16_t> doc_sizes;
		robin_hood::unordered_flat_set<std::string> large_dfs;

		std::vector<uint32_t> csv_line_offsets;
		robin_hood::unordered_map<std::string, std::vector<uint32_t>> inverted_index;

		uint32_t num_docs;
		int      min_df;
		float    avg_doc_size;
		float    max_df;
		float    k1;
		float    b;
		bool     cache_term_freqs;
		bool     cache_inverted_index;

		std::string csv_file;
		std::vector<std::string> columns;

		// LevelDB Management
		// Two databases: one for term frequencies, one for inverted index
		leveldb::DB* doc_term_freqs_db;
		leveldb::DB* inverted_index_db;

		// MMaped vector
		std::vector<int> term_freq_line_offsets;

		_BM25(
				std::vector<std::string>& documents,
				std::string csv_file,
				int   min_df,
				float max_df,
				float k1,
				float b,
				bool cache_term_freqs,
				bool cache_inverted_index
				);

		~_BM25() {
			delete doc_term_freqs_db;
			delete inverted_index_db;
		}

		void get_csv_line_offsets();
		std::vector<std::pair<std::string, std::string>> get_csv_line(int line_num);

		void init_dbs();

		void create_doc_term_freqs_db(
				const robin_hood::unordered_map<std::string, uint32_t>& doc_term_freqs
				);
		void create_inverted_index_db(
				const robin_hood::unordered_map<std::string, std::vector<uint32_t>>& inverted_index
				);
		void write_row_to_inverted_index_db(
				const std::string& term,
				uint32_t doc_id
				);
		uint32_t get_doc_term_freq_db(
				const std::string& term
				);
		std::vector<uint32_t> get_inverted_index_db(
				const std::string& term
				);

		void write_term_freqs_to_file();
		uint16_t get_term_freq_from_file(
				int line_num,
				const std::string& term
				);

		float _compute_idf(
				const std::string& term
				);
		float _compute_bm25(
				const std::string& term,
				uint32_t doc_id,
				float idf
				);

		std::vector<std::pair<uint32_t, float>> query(
				std::string& query,
				uint32_t top_k
				);

		std::vector<std::vector<std::pair<std::string, std::string>>> get_topk_internal(
				std::string& _query,
				uint32_t top_k
				);
};
