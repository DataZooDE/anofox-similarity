# API Reference

> Every function of anofox-similarity, by topic. Each topic page explains the concepts and ends
> with a generated per-function reference (kind, signature, description, examples).

New here? Start with the [getting-started guide](guides/01-getting-started.md), then the
[guides](guides/README.md) for your use case and the runnable [examples](../examples/README.md).

Upgrading? See [what changed](UPGRADING.md). Working with Claude Code? The [skills plugin](../README.md#-claude-code-skills-ai-pair-programming)
condenses this reference into six tested cheat sheets.

## Conventions

- Scalar functions and macros: named arguments bind **by position** on DuckDB 1.5.6. Pass them
  in the documented order (the signatures below show it). Table functions bind by name.
- Table and column parameters take identifiers, optionally qualified (`schema.table`,
  `catalog.schema.table`). They are validated, so they cannot inject SQL.
- Everything works on a read-only database except `compute_fused_embeddings` and
  `compute_transactional_embeddings`, which need the embedding tables (open the database writable
  once). Embeddings are `FLOAT[n]` arrays (vss-compatible). See [conventions](api/00-conventions.md)
  for parameters, errors and performance notes.

## Pages

<!-- generated:functions:start -->

| Page | Topic | Functions |
|---|---|---|
| [00](api/00-conventions.md) | Conventions: naming, parameters, errors, performance | — |
| [01](api/01-similarity-scalars.md) | Similarity scalars: Jaccard, WL kernel, Tversky, containment, weighted measures | [`jaccard_similarity`](api/01-similarity-scalars.md#jaccard_similarity), [`wl_kernel_similarity`](api/01-similarity-scalars.md#wl_kernel_similarity), [`wl_fingerprint`](api/01-similarity-scalars.md#wl_fingerprint), [`tversky_similarity`](api/01-similarity-scalars.md#tversky_similarity), [`containment`](api/01-similarity-scalars.md#containment), [`weighted_jaccard`](api/01-similarity-scalars.md#weighted_jaccard), [`weighted_containment`](api/01-similarity-scalars.md#weighted_containment), [`weighted_tversky`](api/01-similarity-scalars.md#weighted_tversky) |
| [02](api/02-material-search.md) | Material search, predecessors and cold start | [`find_similar_materials_jaccard`](api/02-material-search.md#find_similar_materials_jaccard), [`find_similar_materials_wl_kernel`](api/02-material-search.md#find_similar_materials_wl_kernel), [`infer_predecessors`](api/02-material-search.md#infer_predecessors), [`cold_start_analogs`](api/02-material-search.md#cold_start_analogs) |
| [03](api/03-bom-traversal.md) | BOM traversal and property graph | [`bom_explosion_1level`](api/03-bom-traversal.md#bom_explosion_1level), [`bom_explosion_multilevel`](api/03-bom-traversal.md#bom_explosion_multilevel), [`bom_where_used`](api/03-bom-traversal.md#bom_where_used), [`bom_common_components`](api/03-bom-traversal.md#bom_common_components), [`create_bom_property_graph`](api/03-bom-traversal.md#create_bom_property_graph), [`bom_dfs_neighborhood`](api/03-bom-traversal.md#bom_dfs_neighborhood), [`bom_to_items`](api/03-bom-traversal.md#bom_to_items), [`items_to_materials`](api/03-bom-traversal.md#items_to_materials), [`items_to_bom_header`](api/03-bom-traversal.md#items_to_bom_header), [`items_to_bom_component`](api/03-bom-traversal.md#items_to_bom_component) |
| [04](api/04-embeddings.md) | Embeddings and vector search (textual embeddings are a placeholder) | [`compute_textual_embeddings`](api/04-embeddings.md#compute_textual_embeddings), [`embed_text`](api/04-embeddings.md#embed_text), [`embedding_backend`](api/04-embeddings.md#embedding_backend), [`compute_transactional_embeddings`](api/04-embeddings.md#compute_transactional_embeddings), [`compute_fused_embeddings`](api/04-embeddings.md#compute_fused_embeddings), [`fuse_embeddings`](api/04-embeddings.md#fuse_embeddings), [`compute_jaccard_embeddings`](api/04-embeddings.md#compute_jaccard_embeddings), [`extract_ts_features`](api/04-embeddings.md#extract_ts_features), [`recompute_embedding_statistics`](api/04-embeddings.md#recompute_embedding_statistics), [`check_statistics_freshness`](api/04-embeddings.md#check_statistics_freshness), [`compute_domain_specific_statistics`](api/04-embeddings.md#compute_domain_specific_statistics) |
| [05](api/05-erp-transformations.md) | ERP transformations (SAP, Dynamics 365) | [`sap_to_bom_items`](api/05-erp-transformations.md#sap_to_bom_items), [`sap_to_materials`](api/05-erp-transformations.md#sap_to_materials), [`sap_to_materials_with_desc`](api/05-erp-transformations.md#sap_to_materials_with_desc), [`extract_material_descriptions`](api/05-erp-transformations.md#extract_material_descriptions), [`dynamics365_to_bom_header`](api/05-erp-transformations.md#dynamics365_to_bom_header), [`dynamics365_to_bom_component`](api/05-erp-transformations.md#dynamics365_to_bom_component), [`dynamics365_to_materials`](api/05-erp-transformations.md#dynamics365_to_materials) |
| [06](api/06-infrastructure.md) | Helpers, dependency checks and the tables the extension creates | [`aggregate_material_components`](api/06-infrastructure.md#aggregate_material_components), [`filter_recent_movements`](api/06-infrastructure.md#filter_recent_movements), [`check_anofox_forecast_available`](api/06-infrastructure.md#check_anofox_forecast_available), [`check_duckpgq_available`](api/06-infrastructure.md#check_duckpgq_available) |
| [10](api/10-covering-search.md) | Covering search over feature multisets | [`find_covering_entities`](api/10-covering-search.md#find_covering_entities), [`find_covering_entities_for`](api/10-covering-search.md#find_covering_entities_for) |
| [11](api/11-quantities.md) | Quantities: units, parsing, direction-aware comparison | [`quantity_units`](api/11-quantities.md#quantity_units), [`parse_quantity`](api/11-quantities.md#parse_quantity), [`normalize_quantity`](api/11-quantities.md#normalize_quantity), [`value_satisfies`](api/11-quantities.md#value_satisfies), [`compat_score`](api/11-quantities.md#compat_score) |
| [12](api/12-property-trees.md) | Property trees: load, validate, summarise | [`tree_from_json`](api/12-property-trees.md#tree_from_json), [`validate_trees`](api/12-property-trees.md#validate_trees), [`tree_anchors`](api/12-property-trees.md#tree_anchors), [`tree_class_multiset`](api/12-property-trees.md#tree_class_multiset), [`tree_multiset`](api/12-property-trees.md#tree_multiset) |
| [13](api/13-coverage-kernel.md) | Coverage kernel: ranking with gaps and an explanation | [`tree_coverage`](api/13-coverage-kernel.md#tree_coverage), [`tree_coverage_trace`](api/13-coverage-kernel.md#tree_coverage_trace), [`coverage_recall`](api/13-coverage-kernel.md#coverage_recall), [`coverage_kernel`](api/13-coverage-kernel.md#coverage_kernel) |
| [14](api/14-classification-and-review.md) | Classification onto a catalogue and vocabulary review | [`classify_to_catalogue`](api/14-classification-and-review.md#classify_to_catalogue), [`maxsim`](api/14-classification-and-review.md#maxsim), [`bhattacharyya`](api/14-classification-and-review.md#bhattacharyya), [`calibrated_cosine`](api/14-classification-and-review.md#calibrated_cosine), [`cosine_floor`](api/14-classification-and-review.md#cosine_floor), [`vocabulary_review_queue`](api/14-classification-and-review.md#vocabulary_review_queue), [`apply_vocabulary_review`](api/14-classification-and-review.md#apply_vocabulary_review), [`uncertain_decisions`](api/14-classification-and-review.md#uncertain_decisions) |
| [15](api/15-evaluation.md) | Evaluation and tuning | [`eval_ranking`](api/15-evaluation.md#eval_ranking), [`eval_calibration`](api/15-evaluation.md#eval_calibration), [`threshold_at_precision`](api/15-evaluation.md#threshold_at_precision), [`bootstrap_ci`](api/15-evaluation.md#bootstrap_ci), [`fit_coverage_weights`](api/15-evaluation.md#fit_coverage_weights) |
| [16](api/16-lsh-and-clustering.md) | Sub-linear recall (LSH) and entity clustering | [`weighted_minhash`](api/16-lsh-and-clustering.md#weighted_minhash), [`minhash_similarity`](api/16-lsh-and-clustering.md#minhash_similarity), [`lsh_bands`](api/16-lsh-and-clustering.md#lsh_bands), [`minhash_index`](api/16-lsh-and-clustering.md#minhash_index), [`minhash_bands`](api/16-lsh-and-clustering.md#minhash_bands), [`find_covering_entities_lsh`](api/16-lsh-and-clustering.md#find_covering_entities_lsh), [`connected_components`](api/16-lsh-and-clustering.md#connected_components), [`cluster_matches`](api/16-lsh-and-clustering.md#cluster_matches) |
| [17](api/17-aas.md) | Asset Administration Shell (AAS): read, match on dictionary keys, write | [`semantic_key`](api/17-aas.md#semantic_key), [`aas_to_trees`](api/17-aas.md#aas_to_trees), [`aas_issues`](api/17-aas.md#aas_issues), [`aas_class_hierarchy`](api/17-aas.md#aas_class_hierarchy), [`aas_id_b64url`](api/17-aas.md#aas_id_b64url), [`attrs_satisfying`](api/17-aas.md#attrs_satisfying), [`trees_to_aas`](api/17-aas.md#trees_to_aas), [`aas_export`](api/17-aas.md#aas_export), [`aas_coverage_result`](api/17-aas.md#aas_coverage_result), [`coverage_result_to_aas`](api/17-aas.md#coverage_result_to_aas), [`aas_quality`](api/17-aas.md#aas_quality), [`suggest_semantic_id`](api/17-aas.md#suggest_semantic_id) |

93 functions in total.

<!-- generated:functions:end -->

The reference is generated from the extension's function metadata:
`python3 scripts/generate_api_reference.py` after a release build (`--check` verifies it).
