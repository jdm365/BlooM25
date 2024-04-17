# cython: language_level=3

cimport cython

from libc.stdint cimport uint32_t, uint64_t 
from libcpp.vector cimport vector
from libcpp.string cimport string
from libcpp.pair cimport pair
from libcpp cimport bool

import os


cdef int INT_MAX = 2147483647

cdef extern from "engine.h":
    cdef cppclass _BM25:
        _BM25(
                string filename,
                string search_col,
                int   min_df,
                float max_df,
                float k1,
                float b
                ) nogil
        _BM25(string db_dir)
        _BM25(
                vector[string]& documents,
                int   min_df,
                float max_df,
                float k1,
                float b
                ) nogil
        vector[pair[uint64_t, float]] query(string& term, uint32_t top_k, uint32_t init_max_df)
        vector[vector[pair[string, string]]] get_topk_internal(
                string& term, 
                uint32_t k, 
                uint32_t init_max_df
                )
        void save_to_disk(string db_dir)
        void load_from_disk(string db_dir)



cdef class BM25:
    cdef _BM25* bm25
    cdef int    min_df
    cdef float  max_df
    cdef str    filename 
    cdef str    text_col
    cdef str    db_dir
    cdef float  k1 
    cdef float  b


    def __init__(
            self, 
            int   min_df = 1,
            float max_df = 1.0,
            float k1     = 1.2,
            float b      = 0.4
            ):
        self.min_df = min_df
        self.max_df = max_df
        self.k1     = k1
        self.b      = b


    def index_file(self, str filename, str text_col):
        self.filename = filename
        self.text_col = text_col

        self._init_with_file(filename, text_col)

    def index_documents(self, list documents):
        self._init_with_documents(documents)


    def save(self, db_dir):
        self.db_dir = db_dir

        self.bm25.save_to_disk(db_dir.encode("utf-8"))

        ## Get from os when dir was last modified
        last_modified = os.path.getmtime(self.db_dir)

        ## Write to a file
        with open(os.path.join(self.db_dir, "last_modified.txt"), "w") as f:
            f.write(str(last_modified))

        ## Write to a file
        with open(os.path.join(self.db_dir, "filename.txt"), "w") as f:
            f.write(self.filename)

        if self.filename != "in_memory":
            last_modified = os.path.getmtime(self.filename)
        else:
            last_modified = "in_memory"

        ## Write to a file
        with open(os.path.join(self.db_dir, "last_modified_file.txt"), "w") as f:
            f.write(str(last_modified))


    def load(self, db_dir):
        self.db_dir = db_dir

        ## First check if db_dir exists
        if not os.path.exists(self.db_dir):
            raise RuntimeError("Database directory does not exist")

        ## Check if db_dir has been modified since last save
        with open(os.path.join(self.db_dir, "last_modified.txt"), "r") as f:
            last_modified = f.read()

        new_time = str(os.path.getmtime(self.db_dir))
        new_time = new_time.split('.')[0]
        last_modified = last_modified.split('.')[0]

        if new_time != last_modified:
            raise RuntimeError("Database directory has been modified since last save")

        with open(os.path.join(self.db_dir, "filename.txt"), "r") as f:
            last_filename = f.read()

        if self.filename != last_filename:
            raise RuntimeError("Filename is different from the one used to save the model")

        ## Check if source_file has been modified since last save
        with open(os.path.join(self.db_dir, "last_modified_file.txt"), "r") as f:
            last_modified = f.read()

        if last_modified != "in_memory":
            new_time = str(os.path.getmtime(self.filename))
            new_time = new_time.split('.')[0]
            last_modified = last_modified.split('.')[0]

            if new_time != last_modified:
                raise RuntimeError("Source file has been modified since last save")

        self.bm25 = new _BM25(self.db_dir.encode("utf-8"))
        return True



    cdef void _init_with_documents(self, list documents):
        self.filename = "in_memory"

        cdef vector[string] docs
        docs.reserve(len(documents))
        for doc in documents:
            docs.push_back(doc.upper().encode("utf-8"))

        self.bm25 = new _BM25(
                docs,
                self.min_df,
                self.max_df,
                self.k1,
                self.b
                )

    cdef void _init_with_file(self, str filename, str text_col):
        self.bm25 = new _BM25(
                filename.encode("utf-8"),
                text_col.encode("utf-8"),
                self.min_df,
                self.max_df,
                self.k1,
                self.b
                )

    def get_topk_indices(self, str query, int init_max_df = INT_MAX, int k = 10):
        results = self.bm25.query(query.upper().encode("utf-8"), k, init_max_df)

        scores  = []
        indices = []
        for idx, score in results:
            scores.append(score)
            indices.append(idx)

        return scores, indices


    def get_topk_docs(
            self, 
            str query, 
            int k = 10, 
            int init_max_df = INT_MAX
            ):
        cdef vector[vector[pair[string, string]]] results
        cdef list output = []

        if self.filename == "in_memory":
            raise RuntimeError("""
            Cannot get topk docs when documents were provided instead of a filename
            """)
        else:
            results = self.bm25.get_topk_internal(
                    query.upper().encode("utf-8"), 
                    k, 
                    init_max_df
                    )

        for idx in range(len(results)):
            _dict = {}
            for jdx in range(len(results[idx])):
                _dict[results[idx][jdx].first.decode("utf-8")] = results[idx][jdx].second.decode("utf-8")

            output.append(_dict)

        return output
