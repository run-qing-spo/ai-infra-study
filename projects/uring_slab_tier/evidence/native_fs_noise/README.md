# Native-FS noise evidence

Raw campaign directories are intentionally excluded from Git. Each archived
campaign must retain its upstream `campaign.json`, per-run immutable artifacts,
`summary.json`, and a local SHA-256 index.

Tracked analysis reports may reference a campaign only by its immutable
`campaign_id` and harness `tree_sha256`.
