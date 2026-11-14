#!/usr/bin/env bash
# Smoke test: the AAS the extension writes opens in an AAS server (GitHub #34), and a submodel read
# back over the AAS Part 2 REST API maps to the same tree (GitHub #27).
#
#   scripts/aas_basyx_smoke.sh            # needs docker and the built shell (./build/release/duckdb)
#
# 1. exports the scenario RFQ, the four offers and the coverage results with aas_export /
#    aas_coverage_result,
# 2. starts Eclipse BaSyx (eclipsebasyx/aas-environment, in-memory) and uploads them (POST /upload),
# 3. lists the shells, fetches one submodel by its base64url id (GET /submodels/{id}) and reads it
#    with aas_to_trees, comparing it with the tree it was written from.
# Not part of CI (needs docker); the result is recorded in docs/requirements-matching/REVIEW_LOG.md.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DUCKDB="${DUCKDB:-$ROOT/build/release/duckdb}"
IMAGE="${BASYX_IMAGE:-eclipsebasyx/aas-environment:2.0.0-SNAPSHOT}"
PORT="${BASYX_PORT:-8081}"
WORK="$(mktemp -d)"
NAME="anofox-basyx-smoke-$$"
trap 'docker rm -f "$NAME" >/dev/null 2>&1 || true; rm -rf "$WORK"' EXIT
unset AI_AGENT CLAUDECODE
export DATAZOO_DISABLE_TELEMETRY=1
cd "$ROOT"

echo "== export with the extension"
"$DUCKDB" -c "
CREATE TABLE raw AS
SELECT unnest(aas_to_trees(content, 'RFQ-2026-117')) AS n FROM read_text('test/data/aas/scenario/rfq_2026_117.aas.json')
UNION ALL SELECT unnest(aas_to_trees(content)) FROM read_blob('test/data/aas/scenario/offers/*');
CREATE TABLE tree_nodes AS SELECT n.* EXCLUDE (attributes) FROM raw;
CREATE TABLE tree_attributes AS SELECT unnest(n.attributes, recursive := true) FROM raw;
SET VARIABLE offers = (SELECT list(DISTINCT tree_id) FROM tree_nodes WHERE tree_id <> 'RFQ-2026-117');
CREATE TABLE trace AS SELECT * FROM tree_coverage_trace('RFQ-2026-117', candidates := getvariable('offers'), labels := 'hard');
COPY (SELECT aas_json FROM aas_export(tree_ids := ['RFQ-2026-117'])) TO '$WORK/rfq.aas.json' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '', DELIMITER '\x1f');
COPY (SELECT aas_json FROM aas_export(tree_ids := ['https://lenvaro.example/aas/lx-8'])) TO '$WORK/lenvaro.aas.json' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '', DELIMITER '\x1f');
COPY (SELECT '{\"submodels\": [' || aas_json || ']}' FROM aas_coverage_result('trace') WHERE candidate_id LIKE '%driventa%')
  TO '$WORK/result_driventa.aas.json' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '', DELIMITER '\x1f');
" > /dev/null
ls -la "$WORK"

echo "== start BaSyx ($IMAGE)"
docker run -d --name "$NAME" -p "$PORT:8081" "$IMAGE" > /dev/null
for _ in $(seq 1 90); do
  if curl -fsS "http://localhost:$PORT/shells" > /dev/null 2>&1; then break; fi
  sleep 2
done
curl -fsS "http://localhost:$PORT/shells" > /dev/null

echo "== upload"
for f in rfq lenvaro result_driventa; do
  code=$(curl -s -o "$WORK/$f.upload" -w '%{http_code}' -F "file=@$WORK/$f.aas.json;type=application/json" "http://localhost:$PORT/upload")
  echo "  $f.aas.json -> HTTP $code"
  [ "$code" -lt 300 ] || { cat "$WORK/$f.upload"; exit 1; }
done

echo "== shells in BaSyx"
curl -fsS "http://localhost:$PORT/shells" | python3 -c "import json,sys; [print('  ', s['id']) for s in json.load(sys.stdin)['result']]"

echo "== a submodel back over REST, read with aas_to_trees"
SM_ID="https://lenvaro.example/aas/lx-8/submodels/CapabilityDescription"
B64=$("$DUCKDB" -noheader -list -c "SELECT aas_id_b64url('$SM_ID');")
curl -fsS "http://localhost:$PORT/submodels/$B64" > "$WORK/sm.json"
"$DUCKDB" -c "
CREATE TABLE back AS SELECT unnest(n.attributes, recursive := true) FROM (SELECT unnest(aas_to_trees(content)) AS n FROM read_text('$WORK/sm.json'));
CREATE TABLE orig AS SELECT * FROM (SELECT unnest(n.attributes, recursive := true) FROM (SELECT unnest(aas_to_trees(content)) AS n FROM read_text('test/data/aas/scenario/offers/lenvaro_lx8.aas.json'))) WHERE node_id LIKE 'CapabilityDescription%';
SELECT (SELECT count(*) FROM orig) AS attributes,
       (SELECT count(*) FROM (SELECT * EXCLUDE (tree_id, value_text) FROM orig EXCEPT ALL SELECT * EXCLUDE (tree_id, value_text) FROM back)) AS differences;"
echo "== ok"
