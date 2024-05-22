- Publish to pypi.
- Speed up indexing and reduce memory usage by capping max_df items.
- Make parquet integration more seemless and fast.
- Find heuristics to speed up queries for high df terms.
- Consider allowing ngram tokenization.
- Make tests more robust.
- Add run length encoding to term frequencies before vbyte compression. **Mostly ones there.
- Look into only constructing the compressed index in memory and avoiding the auxiliary structure.