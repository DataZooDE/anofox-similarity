#pragma once

// A MAKT row matches the requested language in either form: SAP's one-character key (SPRAS, 'E')
// or the two-letter ISO code (LAISO, 'EN') that extracts written through the conversion exit carry.
// Cast to VARCHAR: a MAKT whose keys are only digits ('1' Chinese, '3' Korean) may be read as INTEGER.
#define ANOFOX_SPRAS_MATCHES(col) \
	"upper(trim((" col ")::VARCHAR)) IN (upper(trim(COALESCE(language::VARCHAR, 'E'))), " \
	"(MAP {'E': 'EN', 'EN': 'E', 'D': 'DE', 'DE': 'D', 'F': 'FR', 'FR': 'F', 'S': 'ES', 'ES': 'S', 'I': 'IT', 'IT': 'I', 'P': 'PT', 'PT': 'P', 'N': 'NL', 'NL': 'N', 'J': 'JA', 'JA': 'J', '1': 'ZH', 'ZH': '1', 'R': 'RU', 'RU': 'R', 'L': 'PL', 'PL': 'L', 'K': 'DA', 'DA': 'K', 'V': 'SV', 'SV': 'V', 'O': 'NO', 'NO': 'O', 'U': 'FI', 'FI': 'U', 'T': 'TR', 'TR': 'T', 'C': 'CS', 'CS': 'C', 'Q': 'SK', 'SK': 'Q', 'H': 'HU', 'HU': 'H', '3': 'KO', 'KO': '3', 'A': 'AR', 'AR': 'A', 'B': 'HE', 'HE': 'B', 'G': 'EL', 'EL': 'G'})[upper(trim(COALESCE(language::VARCHAR, 'E')))])"
