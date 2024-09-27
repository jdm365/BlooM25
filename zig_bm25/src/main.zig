const std = @import("std");
const progress = @import("progress.zig");
const sorted_array = @import("sorted_array.zig");
const builtin = @import("builtin");

const TOKEN_STREAM_CAPACITY = 1_048_576;
const MAX_LINE_LENGTH       = 1_048_576;
const MAX_NUM_TERMS         = 4096;
const MAX_TERM_LENGTH       = 64;
const MAX_NUM_RESULTS       = 1000;

const AtomicCounter = std.atomic.Value(u64);
const token_t = packed struct(u32) {
    new_doc: u1,
    term_pos: u7,
    doc_id: u24
};

const TermPos = struct {
    start_pos: u32,
    field_len: u32,
};

const QueryResult = struct {
    doc_id: u32,
    score: f32,
    partition_idx: usize,
};

pub fn iterField(buffer: []const u8, byte_pos: *usize) !void {
    // Iterate to next field in compliance with RFC 4180.
    const is_quoted = buffer[byte_pos.*] == '"';
    byte_pos.* += @intFromBool(is_quoted);

    while (true) {
        if (is_quoted) {

            if (buffer[byte_pos.*] == '"') {
                byte_pos.* += 2;
                if (buffer[byte_pos.* - 1] != '"') {
                    return;
                }
            } else {
                byte_pos.* += 1;
            }

        } else {

            switch (buffer[byte_pos.*]) {
                ',' => {
                    byte_pos.* += 1;
                    return;
                },
                '\n' => {
                    return error.UnexpectedNewline;
                },
                else => {
                    byte_pos.* += 1;
                }
            }
        }
    }
}

pub fn parseRecordCSV(
    buffer: []const u8,
    result_positions: []TermPos,
) !void {
    // Parse CSV record in compliance with RFC 4180.
    var byte_pos: usize = 0;
    for (0..result_positions.len) |idx| {
        const start_pos = byte_pos;
        try iterField(buffer, &byte_pos);
        result_positions[idx] = TermPos{
            .start_pos = @intCast(start_pos),
            .field_len = @intCast(byte_pos - start_pos),
        };
    }
}

const TokenStream = struct {
    tokens: []token_t,
    f_data: []align(std.mem.page_size) u8,
    num_terms: u32,
    allocator: std.mem.Allocator,
    output_file: std.fs.File,

    pub fn init(
        filename: []const u8,
        output_filename: []const u8,
        allocator: std.mem.Allocator
    ) !TokenStream {

        const file = try std.fs.cwd().openFile(filename, .{});
        const file_size = try file.getEndPos();

        const output_file = try std.fs.cwd().createFile(output_filename, .{ .read = true });

        const token_stream = TokenStream{
            .tokens = try allocator.alloc(token_t, TOKEN_STREAM_CAPACITY),
            .f_data = try std.posix.mmap(
                null,
                file_size,
                std.posix.PROT.READ,
                .{ .TYPE = .PRIVATE },
                file.handle,
                0
            ),
            .num_terms = 0,
            .allocator = allocator,
            .output_file = output_file,
        };

        return token_stream;
    }

    pub fn deinit(self: *TokenStream) void {
        std.posix.munmap(self.f_data);
        self.allocator.free(self.tokens);
        self.output_file.close();
    }
    
    pub fn addToken(
        self: *TokenStream,
        new_doc: bool,
        term_pos: u8,
        doc_id: u32,
    ) !void {
        self.tokens[self.num_terms] = token_t{
            .new_doc = @intFromBool(new_doc),
            .term_pos = @truncate(term_pos),
            .doc_id = @intCast(doc_id),
        };
        self.num_terms += 1;

        if (self.num_terms == TOKEN_STREAM_CAPACITY) {
            try self.flushTokenStream();
        }
    }

    pub fn flushTokenStream(self: *TokenStream) !void {
        const bytes_to_write = @sizeOf(token_t) * self.num_terms;
        _ = try self.output_file.write(
            std.mem.asBytes(&self.num_terms),
            );
        const bytes_written = try self.output_file.write(
            std.mem.sliceAsBytes(self.tokens[0..self.num_terms])
            );
        
        if (bytes_written != bytes_to_write) {
            return error.WriteError;
        }

        self.num_terms = 0;
    }

    pub fn iterField(self: *TokenStream, byte_pos: *usize) !void {
        // Iterate to next field in compliance with RFC 4180.
        const is_quoted = self.f_data[byte_pos.*] == '"';
        byte_pos.* += @intFromBool(is_quoted);

        while (true) {
            if (is_quoted) {

                if (self.f_data[byte_pos.*] == '"') {
                    byte_pos.* += 2;
                    if (self.f_data[byte_pos.* - 1] != '"') {
                        return;
                    }
                } else {
                    byte_pos.* += 1;
                }

            } else {

                switch (self.f_data[byte_pos.*]) {
                    ',' => {
                        byte_pos.* += 1;
                        return;
                    },
                    '\n' => {
                        return error.UnexpectedNewline;
                    },
                    else => {
                        byte_pos.* += 1;
                    }
                }
            }
        }
    }
};


const InvertedIndex = struct {
    postings: []token_t,
    vocab: std.StringArrayHashMap(u32),
    term_offsets: []u32,
    doc_freqs: std.ArrayList(u32),
    doc_sizes: []u16,

    num_terms: u32,
    num_docs: u32,
    avg_doc_size: f32,

    pub fn init(
        allocator: std.mem.Allocator,
        num_docs: usize,
        ) !InvertedIndex {
        const II = InvertedIndex{
            .postings = &[_]token_t{},
            .vocab = std.StringArrayHashMap(u32).init(allocator),
            .term_offsets = &[_]u32{},
            .doc_freqs = try std.ArrayList(u32).initCapacity(
                allocator, @as(usize, @intFromFloat(@as(f32, @floatFromInt(num_docs)) * 0.1))
                ),
            .doc_sizes = try allocator.alloc(u16, num_docs),
            .num_terms = 0,
            .num_docs = @intCast(num_docs),
            .avg_doc_size = 0.0,
        };
        @memset(II.doc_sizes, 0);
        return II;
    }

    pub fn deinit(
        self: *InvertedIndex,
        allocator: std.mem.Allocator,
        ) void {
        allocator.free(self.postings);
        var iter = self.vocab.iterator();
        while (iter.next()) |entry| {
            allocator.free(entry.key_ptr.*);
        }
        self.vocab.deinit();

        allocator.free(self.term_offsets);
        self.doc_freqs.deinit();
        allocator.free(self.doc_sizes);
    }

    pub fn resizePostings(
        self: *InvertedIndex,
        allocator: std.mem.Allocator,
        ) !void {
        self.num_terms = @intCast(self.doc_freqs.items.len);
        self.term_offsets = try allocator.alloc(u32, self.num_terms);

        // Num terms is now known.
        var postings_size: usize = 0;
        for (0.., self.doc_freqs.items) |i, doc_freq| {
            self.term_offsets[i] = @intCast(postings_size);
            postings_size += doc_freq;
        }
        self.term_offsets[self.num_terms - 1] = @intCast(postings_size);
        self.postings = try allocator.alloc(token_t, postings_size + 1);

        var avg_doc_size: f64 = 0.0;
        for (self.doc_sizes) |doc_size| {
            avg_doc_size += @floatFromInt(doc_size);
        }
        avg_doc_size /= @floatFromInt(self.num_docs);
        self.avg_doc_size = @floatCast(avg_doc_size);
    }
};

const BM25Partition = struct {
    II: []InvertedIndex,
    line_offsets: []usize,
    allocator: std.mem.Allocator,

    pub fn init(
        allocator: std.mem.Allocator,
        num_search_cols: usize,
        line_offsets: []usize,
    ) !BM25Partition {
        const partition = BM25Partition{
            .II = try allocator.alloc(InvertedIndex, num_search_cols),
            .line_offsets = line_offsets,
            .allocator = allocator,
        };

        for (0..num_search_cols) |idx| {
            partition.II[idx] = try InvertedIndex.init(allocator, line_offsets.len);
        }

        return partition;
    }

    pub fn deinit(self: *BM25Partition) void {
        self.allocator.free(self.line_offsets);
        for (0..self.II.len) |i| {
            self.II[i].deinit(self.allocator);
        }
        self.allocator.free(self.II);
    }

    pub fn constructFromTokenStream(
        self: *BM25Partition,
        token_streams: *[]TokenStream,
        ) !void {

        for (0.., self.II) |col_idx, *II| {
            try II.resizePostings(self.allocator);
            var term_offsets = try self.allocator.alloc(usize, II.num_terms);
            defer self.allocator.free(term_offsets);
            @memset(term_offsets, 0);

            // Create index.
            const ts = token_streams.*[col_idx];
            try ts.output_file.seekTo(0);

            var bytes_read: usize = 0;

            var num_tokens: usize = TOKEN_STREAM_CAPACITY;
            var current_doc_id: usize = 0;

            while (num_tokens == TOKEN_STREAM_CAPACITY) {
                var _num_tokens: [4]u8 = undefined;
                _ = try ts.output_file.read(std.mem.asBytes(&_num_tokens));
                const endianness = builtin.cpu.arch.endian();
                num_tokens = std.mem.readInt(u32, &_num_tokens, endianness);

                bytes_read = try ts.output_file.read(
                    std.mem.sliceAsBytes(ts.tokens[0..num_tokens])
                    );
                std.debug.assert(bytes_read == 4 * num_tokens);

                var token_count: usize = 0;
                for (token_count..token_count + num_tokens) |idx| {
                    const new_doc  = ts.tokens[idx].new_doc;
                    const term_pos = ts.tokens[idx].term_pos;
                    const term_id: usize = @intCast(ts.tokens[idx].doc_id);

                    current_doc_id += @intCast(new_doc);

                    const token = token_t{
                        .new_doc = 0,
                        .term_pos = term_pos,
                        .doc_id = @intCast(current_doc_id),
                    };

                    const postings_offset = II.term_offsets[term_id] + term_offsets[term_id];
                    std.debug.assert(postings_offset < II.postings.len);

                    term_offsets[term_id] += 1;

                    II.postings[postings_offset] = token;
                }

                token_count += num_tokens;

                _ = try ts.output_file.read(std.mem.asBytes(&_num_tokens));
                num_tokens = std.mem.readInt(u32, &_num_tokens, endianness);

                bytes_read = try ts.output_file.read(
                    std.mem.sliceAsBytes(ts.tokens[0..num_tokens])
                    );
                }
        }
    }

    pub fn fetchRecords(
        self: *const BM25Partition,
        result_positions: []TermPos,
        file_handle: *std.fs.File,
        query_result: QueryResult,
        record_string: *std.ArrayList(u8),
    ) !void {
        std.debug.print("\n\n\n\nQuerying documents\n\n\n\n", .{});
        const doc_id: usize = @intCast(query_result.doc_id);
        const byte_offset = self.line_offsets[doc_id];
        const next_byte_offset = self.line_offsets[doc_id + 1];
        const bytes_to_read = next_byte_offset - byte_offset;

        _ = try file_handle.seekTo(byte_offset);
        if (bytes_to_read > record_string.capacity) {
            try record_string.ensureTotalCapacity(bytes_to_read);
        }
        const bytes_read = try file_handle.read(
            std.mem.asBytes(record_string.items[0..bytes_to_read])
            );

        std.debug.assert(bytes_read == bytes_to_read);

        try parseRecordCSV(record_string.items[0..bytes_read], result_positions);
    }
};



fn addTerm(
    term: []u8,
    term_len: usize,
    doc_id: u32,
    term_pos: u8,
    index_partition: *BM25Partition,
    col_idx: usize,
    token_stream: *TokenStream,
    terms_seen: *std.bit_set.StaticBitSet(MAX_NUM_TERMS),
    allocator: std.mem.Allocator,
    new_doc: *bool,
) !void {
    const gop = try index_partition.II[col_idx].vocab.getOrPut(term[0..term_len]);
    if (!gop.found_existing) {
        const term_copy = try allocator.dupe(u8, term[0..term_len]);
        errdefer allocator.free(term_copy);

        gop.key_ptr.* = term_copy;
        gop.value_ptr.* = index_partition.II[col_idx].num_terms;
        index_partition.II[col_idx].num_terms += 1;
        try index_partition.II[col_idx].doc_freqs.append(1);
        try token_stream.addToken(new_doc.*, term_pos, gop.value_ptr.*);
    } else {
        if (!terms_seen.isSet(gop.value_ptr.* % MAX_NUM_TERMS)) {
            index_partition.II[col_idx].doc_freqs.items[gop.value_ptr.*] += 1;
            try token_stream.addToken(new_doc.*, term_pos, gop.value_ptr.*);
        }
    }

    index_partition.II[col_idx].doc_sizes[doc_id] += 1;
    new_doc.* = false;
}

pub fn processDocRfc4180(
    token_stream: *TokenStream,
    index_partition: *BM25Partition,
    doc_id: u32,
    byte_idx: *usize,
    col_idx: usize,
    term: *[MAX_TERM_LENGTH]u8,
    max_byte: usize,
) !void {
    var term_pos: u8 = 0;
    const is_quoted  = (token_stream.f_data[byte_idx.*] == '"');
    byte_idx.* += @intFromBool(is_quoted);

    var cntr: usize = 0;
    var new_doc = (doc_id != 0);

    var terms_seen: std.bit_set.StaticBitSet(MAX_NUM_TERMS) = undefined;

    if (is_quoted) {

        while (true) {
            std.debug.assert(index_partition.II[col_idx].doc_sizes[doc_id] < MAX_NUM_TERMS);
            std.debug.assert(byte_idx.* < max_byte);

            if (token_stream.f_data[byte_idx.*] == '"') {
                byte_idx.* += 1;

                if ((token_stream.f_data[byte_idx.*] == ',') or (token_stream.f_data[byte_idx.*] == '\n')) {
                    break;
                }

                // Double quote means escaped quote. Opt not to include in token for now.
                if (token_stream.f_data[byte_idx.*] == '"') {
                    byte_idx.* += 1;
                    continue;
                }
            }

            if ((token_stream.f_data[byte_idx.*] == ' ') or (cntr == MAX_TERM_LENGTH - 1)) {
                if (cntr == 0) {
                    byte_idx.* += 1;
                    continue;
                }

                try addTerm(
                    term, 
                    cntr, 
                    doc_id, 
                    term_pos, 
                    index_partition, 
                    col_idx, 
                    token_stream, 
                    &terms_seen,
                    index_partition.allocator,
                    &new_doc,
                    );

                if (term_pos != 255) {
                    term_pos += 1;
                }
                cntr = 0;
                byte_idx.* += 1;
                continue;
            }

            term[cntr] = std.ascii.toUpper(token_stream.f_data[byte_idx.*]);
            cntr += 1;
            byte_idx.* += 1;
        }

    } else {

        while (true) {
            std.debug.assert(index_partition.II[col_idx].doc_sizes[doc_id] < MAX_NUM_TERMS);
            std.debug.assert(byte_idx.* < max_byte);

            if ((token_stream.f_data[byte_idx.*] == ',') or (token_stream.f_data[byte_idx.*] == '\n')) {
                break;
            }

            if ((token_stream.f_data[byte_idx.*] == ' ') or (cntr == MAX_TERM_LENGTH - 1)) {
                if (cntr == 0) {
                    byte_idx.* += 1;
                    continue;
                }

                try addTerm(
                    term, 
                    cntr, 
                    doc_id, 
                    term_pos, 
                    index_partition, 
                    col_idx, 
                    token_stream, 
                    &terms_seen,
                    index_partition.allocator,
                    &new_doc,
                    );

                cntr = 0;
                byte_idx.* += 1;

                if (term_pos != 255) {
                    term_pos += 1;
                }
                continue;
            }

            term[cntr] = std.ascii.toUpper(token_stream.f_data[byte_idx.*]);
            cntr += 1;
            byte_idx.* += 1;
        }
    }

    if (cntr > 0) {
        std.debug.assert(index_partition.II[col_idx].doc_sizes[doc_id] < MAX_NUM_TERMS);

        try addTerm(
            term, 
            cntr, 
            doc_id, 
            term_pos, 
            index_partition, 
            col_idx, 
            token_stream, 
            &terms_seen,
            index_partition.allocator,
            &new_doc,
            );
    }

    byte_idx.* += 1;
}


const IndexManager = struct {
    index_partitions: []BM25Partition,
    input_filename: []const u8,
    allocator: std.mem.Allocator,
    search_cols: std.StringArrayHashMap(u16),
    num_cols: usize,
    file_handles: []std.fs.File,
    tmp_dir: []const u8,
    result_positions: [MAX_NUM_RESULTS][]TermPos,
    result_strings: [MAX_NUM_RESULTS]std.ArrayList(u8),

    pub fn init(
        input_filename: []const u8,
        search_cols: *std.ArrayList([]const u8),
        allocator: std.mem.Allocator,
        ) !IndexManager {

        var num_cols: usize = 0;

        const search_col_map = try readCSVHeader(
            input_filename, 
            search_cols, 
            &num_cols,
            allocator,
            );
        // const num_partitions = try std.Thread.getCpuCount();
        const num_partitions = 1;

        std.debug.print("Writing {d} partitions\n", .{num_partitions});

        const file_hash = blk: {
            var hash: [std.crypto.hash.sha2.Sha256.digest_length]u8 = undefined;
            std.crypto.hash.sha2.Sha256.hash(input_filename, &hash, .{});
            break :blk hash;
        };
        const dir_name = try std.fmt.allocPrint(
            allocator,
            ".{x:0>32}", .{std.fmt.fmtSliceHexLower(file_hash[0..16])}
            );

        std.fs.cwd().makeDir(dir_name) catch {
            try std.fs.cwd().deleteTree(dir_name);
            try std.fs.cwd().makeDir(dir_name);
        };

        // init file handles
        const file_handles = try allocator.alloc(std.fs.File, num_partitions);
        for (0..num_partitions) |idx| {
            file_handles[idx] = try std.fs.cwd().openFile(input_filename, .{});
        }

        var result_positions: [MAX_NUM_RESULTS][]TermPos = undefined;
        var result_strings: [MAX_NUM_RESULTS]std.ArrayList(u8) = undefined;
        for (0..MAX_NUM_RESULTS) |idx| {
            result_positions[idx] = try allocator.alloc(TermPos, num_cols);
            result_strings[idx] = try std.ArrayList(u8).initCapacity(allocator, 4096);
        }

        return IndexManager{
            .index_partitions = try allocator.alloc(BM25Partition, num_partitions),
            .input_filename = input_filename,
            .allocator = allocator,
            .search_cols = search_col_map,
            .num_cols = num_cols,
            .tmp_dir = dir_name,
            .file_handles = file_handles,
            .result_positions = result_positions,
            .result_strings = result_strings,
        };
    }
    
    pub fn deinit(self: *IndexManager) !void {
        for (0..self.index_partitions.len) |i| {
            self.index_partitions[i].deinit();
            self.file_handles[i].close();
        }
        self.allocator.free(self.file_handles);
        self.allocator.free(self.index_partitions);

        self.search_cols.deinit();

        try std.fs.cwd().deleteTree(self.tmp_dir);
        self.allocator.free(self.tmp_dir);

        for (0..MAX_NUM_RESULTS) |idx| {
            self.allocator.free(self.result_positions[idx]);
            self.result_strings[idx].deinit();
        }

    }

    fn readCSVHeader(
        input_filename: []const u8,
        search_cols: *std.ArrayList([]const u8),
        num_cols: *usize,
        allocator: std.mem.Allocator,
        ) !std.StringArrayHashMap(u16) {
        var col_map = std.StringArrayHashMap(u16).init(allocator);
        var col_idx: usize = 0;
        num_cols.* = 0;

        const file = try std.fs.cwd().openFile(input_filename, .{});
        defer file.close();

        const file_size = try file.getEndPos();

        const f_data = try std.posix.mmap(
            null,
            file_size,
            std.posix.PROT.READ,
            .{ .TYPE = .PRIVATE },
            file.handle,
            0
        );
        defer std.posix.munmap(f_data);

        var term: [MAX_TERM_LENGTH]u8 = undefined;
        var cntr: usize = 0;

        var byte_pos: usize = 0;
        line: while (true) {
            std.debug.assert(byte_pos < file_size);
            const is_quoted = f_data[byte_pos] == '"';
            byte_pos += @intFromBool(is_quoted);

            while (true) {
                if (is_quoted) {

                    if (f_data[byte_pos] == '"') {
                        byte_pos += 2;
                        if (f_data[byte_pos - 1] != '"') {
                            // ADD TERM
                            for (0..search_cols.items.len) |i| {
                                if (std.mem.eql(u8, term[0..cntr], search_cols.items[i])) {
                                    try col_map.put(term[0..cntr], @intCast(col_idx));
                                    break;
                                }
                            }
                            cntr = 0;
                            byte_pos += 1;
                            col_idx += 1;
                            continue :line;
                        }
                    } else {
                        // Add char.
                        term[cntr] = std.ascii.toUpper(f_data[byte_pos]);
                        cntr += 1;
                        byte_pos += 1;
                    }

                } else {

                    switch (f_data[byte_pos]) {
                        ',' => {
                            // ADD TERM.
                            for (0..search_cols.items.len) |i| {
                                if (std.mem.eql(u8, term[0..cntr], search_cols.items[i])) {
                                    try col_map.put(term[0..cntr], @intCast(col_idx));
                                    break;
                                }
                            }
                            cntr = 0;
                            byte_pos += 1;
                            col_idx += 1;
                            continue :line;
                        },
                        '\n' => {
                            // ADD TERM.
                            return col_map;
                        },
                        else => {
                            // Add char
                            term[cntr] = std.ascii.toUpper(f_data[byte_pos]);
                            cntr += 1;
                            byte_pos += 1;
                        }
                    }
                }
            }

            col_idx += 1;
        }

        num_cols.* = col_idx;
    }
        


    fn readPartition(
        self: *IndexManager,
        partition_idx: usize,
        chunk_size: usize,
        final_chunk_size: usize,
        num_partitions: usize,
        line_offsets: *const std.ArrayList(usize),
        partition_boundaries: *const std.ArrayList(usize),
        num_cols: usize,
        search_col_idxs: []u16,
        total_docs_read: *AtomicCounter,
        progress_bar: *progress.ProgressBar,
    ) !void {
        var timer = try std.time.Timer.start();
        const interval_ns: u64 = 1_000_000_000 / 30;

        var term_buffer: [MAX_TERM_LENGTH]u8 = undefined;

        const current_chunk_size = switch (partition_idx != num_partitions - 1) {
            true => chunk_size,
            false => final_chunk_size,
        };

        const end_pos   = partition_boundaries.items[partition_idx + 1];
        const start_doc = partition_idx * chunk_size;
        const end_doc   = start_doc + current_chunk_size;

        var token_streams = try self.allocator.alloc(TokenStream, num_cols);

        for (0..num_cols) |col_idx| {
            const output_filename = try std.fmt.allocPrint(
                self.allocator, 
                "{s}/output_{d}_{d}.bin", 
                .{self.tmp_dir, partition_idx, col_idx}
                );
            defer self.allocator.free(output_filename);
            token_streams[col_idx] = try TokenStream.init(
                self.input_filename,
                output_filename,
                self.allocator,
            );
        }
        defer {
            for (0..num_cols) |col_idx| {
                token_streams[col_idx].deinit();
            }
            self.allocator.free(token_streams);
        }

        var last_doc_id: usize = 0;
        for (0.., start_doc..end_doc) |doc_id, _| {

            if (timer.read() >= interval_ns) {
                const current_docs_read = total_docs_read.fetchAdd(
                    doc_id - last_doc_id, 
                    .monotonic
                    ) + (doc_id - last_doc_id);
                last_doc_id = doc_id;
                timer.reset();

                if (partition_idx == 0) {
                    progress_bar.update(current_docs_read);
                }
            }

            var line_offset = line_offsets.items[doc_id];

            const next_line_offset = switch (doc_id == line_offsets.items.len - 1) {
                true => end_pos,
                false => line_offsets.items[doc_id + 1],
            };

            var search_col_idx: usize = 0;
            var prev_col: usize = 0;

            while (search_col_idx < num_cols) {

                for (prev_col..search_col_idxs[search_col_idx]) |_| {
                    try token_streams[0].iterField(&line_offset);
                }

                std.debug.assert(line_offset < next_line_offset);
                try processDocRfc4180(
                    &token_streams[search_col_idx],
                    &self.index_partitions[partition_idx],
                    @intCast(doc_id), 
                    &line_offset, 
                    search_col_idx,
                    &term_buffer,
                    next_line_offset,
                    );
                prev_col = search_col_idxs[search_col_idx];
                search_col_idx += 1;
            }
        }

        // Flush remaining tokens.
        for (token_streams) |*stream| {
            try stream.flushTokenStream();
        }
        _ = total_docs_read.fetchAdd(end_doc - start_doc - last_doc_id, .monotonic);

        // Construct II
        try self.index_partitions[partition_idx].constructFromTokenStream(&token_streams);
    }


    pub fn readFile(self: *IndexManager) !void {
        const file = try std.fs.cwd().openFile(self.input_filename, .{});
        defer file.close();

        const file_size = try file.getEndPos();

        const f_data = try std.posix.mmap(
            null,
            file_size,
            std.posix.PROT.READ,
            .{ .TYPE = .PRIVATE },
            file.handle,
            0
        );
        defer std.posix.munmap(f_data);

        var line_offsets = std.ArrayList(usize).init(self.allocator);
        defer line_offsets.deinit();

        var file_pos: usize = 0;

        // Time read.
        const start_time = std.time.milliTimestamp();

        while (file_pos < file_size - 1) {

            switch (f_data[file_pos]) {
                '"' => {
                    // Iter over quote
                    file_pos += 1;

                    while (true) {

                        if (f_data[file_pos] == '"') {
                            // Escape quote. Continue to next character.
                            if (f_data[file_pos + 1] == '"') {
                                file_pos += 2;
                                continue;
                            }

                            // Iter over quote.
                            file_pos += 1;
                            break;
                        }

                        file_pos += 1;
                    }
                },
                '\n' => {
                    file_pos += 1;
                    try line_offsets.append(file_pos);
                },
                else => file_pos += 1,
            }
        }

        const end_time = std.time.milliTimestamp();
        const execution_time_ms = end_time - start_time;
        const mb_s: usize = @as(usize, @intFromFloat(0.001 * @as(f32, @floatFromInt(file_size)) / @as(f32, @floatFromInt(execution_time_ms))));

        std.debug.print("Read {d} lines in {d}ms\n", .{line_offsets.items.len, execution_time_ms});
        std.debug.print("{d}MB/s\n", .{mb_s});

        const num_lines = line_offsets.items.len;
        const num_partitions = self.index_partitions.len;
        const chunk_size: usize = num_lines / num_partitions;
        const final_chunk_size: usize = chunk_size + (num_lines % num_partitions);

        var partition_boundaries = std.ArrayList(usize).init(self.allocator);
        defer partition_boundaries.deinit();

        const num_search_cols = self.search_cols.count();

        for (0..num_partitions) |i| {
            try partition_boundaries.append(line_offsets.items[i * chunk_size]);

            const current_chunk_size = switch (i != num_partitions - 1) {
                true => chunk_size,
                false => final_chunk_size,
            };

            const start = i * chunk_size;
            const end = start + current_chunk_size;

            const partition_line_offsets = try self.allocator.alloc(usize, current_chunk_size);
            @memcpy(partition_line_offsets, line_offsets.items[start..end]);

            self.index_partitions[i] = try BM25Partition.init(
                self.allocator, 
                num_search_cols, 
                partition_line_offsets
                );
        }
        try partition_boundaries.append(file_size);

        std.debug.assert(partition_boundaries.items.len == num_partitions + 1);

        const search_col_idxs = self.search_cols.values();
        const time_start = std.time.milliTimestamp();

        var threads = try self.allocator.alloc(std.Thread, num_partitions);
        defer self.allocator.free(threads);

        var total_docs_read = AtomicCounter.init(0);
        var progress_bar = progress.ProgressBar.init(line_offsets.items.len);

        for (0..num_partitions) |partition_idx| {

            threads[partition_idx] = try std.Thread.spawn(
                .{},
                readPartition,
                .{
                    self,
                    partition_idx,
                    chunk_size,
                    final_chunk_size,
                    num_partitions,
                    &line_offsets,
                    &partition_boundaries,
                    num_search_cols,
                    search_col_idxs,
                    &total_docs_read,
                    &progress_bar,
                    },
                );
        }

        for (threads) |thread| {
            thread.join();
        }

        const _total_docs_read = total_docs_read.load(.acquire);
        std.debug.assert(_total_docs_read == line_offsets.items.len);
        progress_bar.update(_total_docs_read);

        const time_end = std.time.milliTimestamp();
        const time_diff = time_end - time_start;
        std.debug.print("Processed {d} documents in {d}ms\n", .{line_offsets.items.len, time_diff});
    }


    pub fn queryPartition(
        self: *const IndexManager,
        queries: std.StringHashMap([]const u8),
        boost_factors: std.ArrayList(f32),
        partition_idx: usize,
        query_results: *sorted_array.SortedScoreArray(QueryResult),
    ) !void {
        const num_search_cols = self.search_cols.count();
        std.debug.assert(num_search_cols > 0);

        // Tokenize query.
        var tokens: []std.ArrayList(u32) = try self.allocator.alloc(
            std.ArrayList(u32), 
            num_search_cols
            );
        for (tokens) |*t| {
            t.* = std.ArrayList(u32).init(self.allocator);
        }
        defer {
            for (tokens) |*t| {
                t.deinit();
            }
            self.allocator.free(tokens);
        }

        var term_buffer: [MAX_TERM_LENGTH]u8 = undefined;

        var empty_query = true; 

        var col: usize = 0;

        var query_it = queries.iterator();
        while (query_it.next()) |entry| {
            const _col_idx = self.search_cols.get(entry.key_ptr.*);
            if (_col_idx == null) continue;
            const col_idx = _col_idx.?;

            // tokens[col] = try std.ArrayList(u32).init(self.allocator);

            var term_len: usize = 0;

            for (0.., entry.value_ptr.*) |idx, c| {
                if (c == ' ') {
                    if (term_len == 0) continue;

                    const token = self.index_partitions[partition_idx].II[col_idx].vocab.get(
                        entry.key_ptr.*[idx-term_len..idx]
                        );
                    if (token != null) {
                        try tokens[col_idx].append(token.?);
                        empty_query = false;
                    }
                    term_len = 0;
                    continue;
                }

                term_buffer[term_len] = std.ascii.toUpper(c);
                term_len += 1;

                if (term_len == MAX_TERM_LENGTH) {
                    const token = self.index_partitions[partition_idx].II[col_idx].vocab.get(
                        entry.key_ptr.*[idx-term_len..idx]
                        );
                    if (token != null) {
                        try tokens[col_idx].append(token.?);
                        empty_query = false;
                    }
                    term_len = 0;
                }
            }

            if (term_len > 0) {
                const token = self.index_partitions[partition_idx].II[col_idx].vocab.get(
                    entry.key_ptr.*[entry.key_ptr.*.len-term_len..entry.key_ptr.*.len]
                    );
                if (token != null) {
                    try tokens[col_idx].append(token.?);
                    empty_query = false;
                }
            }

            col += 1;
        }

        // if (empty_query) return;
        if (empty_query) {
            std.debug.print("Empty query\n", .{});
            std.debug.assert(query_results.count == 0);
            return;
        }

        // For each token in each II, get relevant docs and add to score.
        var doc_scores = std.AutoArrayHashMap(u32, f32).init(self.allocator);

        for (0.., tokens) |col_idx, col_tokens| {
            const II = self.index_partitions[partition_idx].II[col_idx];

            for (col_tokens.items) |token| {
                const idf: f32 = 1 + @as(f32, @floatFromInt(std.math.log2(II.num_docs / II.doc_freqs.items[token])));

                const last_offset = if (token == II.num_terms - 1) II.postings.len else II.term_offsets[token + 1];
                for (II.postings[II.term_offsets[token]..last_offset]) |doc_token| {
                    const score = idf * boost_factors.items[col_idx];

                    const doc_id:   u32 = @intCast(doc_token.doc_id);
                    // const term_pos: u32 = @intCast(doc_token.term_pos);

                    const result = try doc_scores.getOrPut(doc_id);
                    if (result.found_existing) {
                        result.value_ptr.* += score;
                    } else {
                        result.key_ptr.* = doc_id;
                        result.value_ptr.* = score;
                    }
                }
            }
        }

        var score_it = doc_scores.iterator();
        while (score_it.next()) |entry| {
            const score_pair = QueryResult{
                .doc_id = entry.key_ptr.*,
                .score = entry.value_ptr.*,
                .partition_idx = partition_idx,
            };
            query_results.insert(score_pair);
        }
    }


    pub fn query(
        self: *const IndexManager,
        queries: std.StringHashMap([]const u8),
        k: usize,
        boost_factors: std.ArrayList(f32),
    ) ![MAX_NUM_RESULTS]std.ArrayList(u8) {
        if (k > MAX_NUM_RESULTS) {
            std.debug.print("k must be less than or equal to {d}\n", .{MAX_NUM_RESULTS});
            return error.InvalidArgument;
        }

        // Init num_partitions threads.
        const num_partitions = self.index_partitions.len;
        var threads = try self.allocator.alloc(std.Thread, num_partitions);
        defer self.allocator.free(threads);

        var thread_results = try self.allocator.alloc(
            sorted_array.SortedScoreArray(QueryResult), 
            num_partitions
            );
        defer {
            for (thread_results) |*res| {
                res.deinit();
            }
            self.allocator.free(thread_results);
        }

        for (0..num_partitions) |partition_idx| {
            thread_results[partition_idx] = try sorted_array.SortedScoreArray(QueryResult).init(self.allocator, k);
            threads[partition_idx] = try std.Thread.spawn(
                .{},
                queryPartition,
                .{
                    self,
                    queries,
                    boost_factors,
                    partition_idx,
                    &thread_results[partition_idx],
                },
            );
        }

        var results = try sorted_array.SortedScoreArray(QueryResult).init(self.allocator, k);
        defer results.deinit();

        for (threads) |thread| {
            thread.join();
        }

        for (thread_results) |*tr| {
            for (tr.items[0..tr.count]) |r| {
                results.insert(r);
            }
        }

        if (results.count == 0) return self.result_strings;

        for (0..results.count) |idx| {
            const result = results.items[idx];

            std.debug.print("partition_idx: {d}\n", .{result.partition_idx});

            threads[result.partition_idx] = try std.Thread.spawn(
                .{},
                BM25Partition.fetchRecords,
                .{
                    &self.index_partitions[result.partition_idx],
                    self.result_positions[idx],
                    &self.file_handles[result.partition_idx],
                    result,
                    @constCast(&self.result_strings[idx]),
                },
            );
        }

        for (threads) |thread| {
            thread.join();
        }

        return self.result_strings;
    }
};


pub fn main() !void {
    const filename: []const u8 = "../tests/mb_small.csv";

    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    const allocator = blk: {
        if (builtin.mode == .ReleaseFast) {
            break :blk std.heap.c_allocator;
        }
        break :blk gpa.allocator();
    }; 

    var search_cols = std.ArrayList([]const u8).init(allocator);
    try search_cols.append("TITLE");
    try search_cols.append("ARTIST");

    var index_manager = try IndexManager.init(filename, &search_cols, allocator);
    try index_manager.readFile();

    defer {
        search_cols.deinit();
        index_manager.deinit() catch {};
        _ = gpa.deinit();
    }

    var query_map = std.StringHashMap([]const u8).init(allocator);
    defer query_map.deinit();

    try query_map.put("TITLE", "UNDER MY SKIN");
    try query_map.put("ARTIST", "FRANK SINATRA");

    var boost_factors = std.ArrayList(f32).init(allocator);
    defer boost_factors.deinit();

    try boost_factors.append(2.0);
    try boost_factors.append(1.0);

    _ = try index_manager.query(
        query_map,
        10,
        boost_factors,
        );

    std.debug.print("Query complete\n", .{});
}
