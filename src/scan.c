// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * src/scan.c
 *
 * Scan-related subroutines
 *
 * Copyright (C) 2018-2020 SCANOSS.COM
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "scan.h"
#include "snippets.h"
#include "match.h"
#include "query.h"
#include "file.h"
#include "util.h"
#include "parse.h"
#include "debug.h"
#include "psi.h"
#include "limits.h"
#include "blacklist.h"
#include "winnowing.h"
#include "ldb.h"

char *sbom = NULL;
char *blacklisted_assets = NULL;

/* Calculate and write source wfp md5 in scan->source_md5 */
static void calc_wfp_md5(scan_data *scan)
{
	uint8_t tmp_md5[16];
	file_md5(scan->file_path, tmp_md5);
	char *tmp_md5_hex = md5_hex(tmp_md5);
	strcpy(scan->source_md5, tmp_md5_hex);
	free(tmp_md5_hex);
}

/* Init scan structure */
scan_data scan_data_init(char *target)
{
	scan_data scan;
	scan.md5 = calloc (MD5_LEN,1);
	scan.file_path = calloc(LDB_MAX_REC_LN, 1);
	strcpy(scan.file_path, target);

	scan.file_size = calloc(LDB_MAX_REC_LN, 1);

	strcpy(scan.source_md5, "00000000000000000000000000000000\0");
	scan.hashes = malloc(MAX_FILE_SIZE);
	scan.lines  = malloc(MAX_FILE_SIZE);
	scan.hash_count = 0;
	scan.timer = 0;
	scan.preload = false;
	scan.total_lines = 0;
	scan.matchmap = calloc(MAX_FILES, sizeof(matchmap_entry));
	scan.matchmap_size = 0;
	scan.match_type = none;
	scan.preload = false;

	/* Get wfp MD5 hash */
	if (extension(target)) if (!strcmp(extension(target), "wfp")) calc_wfp_md5(&scan);

	return scan;
}

static void scan_data_reset(scan_data *scan)
{
	*scan->file_path = 0;
	*scan->file_size = 0;
	scan->hash_count = 0;
	scan->timer = 0;
	scan->total_lines = 0;
	scan->matchmap_size = 0;
	scan->hash_count = 0;
	scan->match_type = none;
}

void scan_data_free(scan_data scan)
{
	free(scan.md5);
	free(scan.file_path);
	free(scan.file_size);
	free(scan.hashes);
	free(scan.lines);
	free(scan.matchmap);
}

/* Returns true if md5 is the md5sum for NULL */
static bool zero_bytes (uint8_t *md5)
{
	uint8_t empty[] = "\xd4\x1d\x8c\xd9\x8f\x00\xb2\x04\xe9\x80\x09\x98\xec\xf8\x42\x7e";

	for (int i = 0; i < 15; i++)
		if (md5[i] != empty[i]) return false;

	return true;
}

/* Performs component and file comparison */
static matchtype ldb_scan_file(uint8_t *fid) {
			
	scanlog("Checking entire file\n");
	
	if (zero_bytes(fid)) return none;
	
	matchtype match_type = none;

	if (ldb_key_exists(oss_component, fid)) match_type = component;
	else if (ldb_key_exists(oss_file, fid)) match_type = file;

	return match_type;
}

bool assets_match(match_data match)
{
	if (!sbom) return false;

	bool found = false;	

	char *asset = calloc(LDB_MAX_REC_LN, 1);
	sprintf(asset, "%s,", match.component);

	if (strstr(sbom, asset)) found = true;
	free(asset);

	return found;
}

bool blacklist_match(uint8_t *component_record)
{
	if (!blacklisted_assets) return false;

	bool found = false;	

	char *asset = calloc(LDB_MAX_REC_LN, 1);
	extract_csv(asset, (char *) component_record, 2, LDB_MAX_REC_LN);
	strcat(asset, ",");

	if (strcasestr(blacklisted_assets, asset)) found = true;
	free(asset);

	if (found) scanlog("Component blacklisted: %s\n", component_record);

	return found;
}

match_data fill_match(uint8_t *file_record, uint8_t *component_record)
{
	match_data match;
	match.selected = false;
	match.path_ln = 0;

	/* Extract fields from file record */
	if (file_record)
	{
		memcpy(match.component_md5, file_record, MD5_LEN);
		strcpy(match.file, (char *) file_record + MD5_LEN);
		match.path_ln = strlen((char *) file_record + MD5_LEN);
	}
	else strcpy(match.file, "all");

	/* Extract fields from url record */
	extract_csv(match.vendor,    (char *) component_record, 1, sizeof(match.vendor));
	extract_csv(match.component, (char *) component_record, 2, sizeof(match.component));
	extract_csv(match.version,   (char *) component_record, 3, sizeof(match.version));
	extract_csv(match.url,       (char *) component_record, 4, sizeof(match.url));
	strcpy(match.latest_version, match.version);

	flip_slashes(match.vendor);
	flip_slashes(match.component);
	flip_slashes(match.version);
	flip_slashes(match.url);
	flip_slashes(match.file);

	if (!*match.vendor || !*match.component || !*match.url || !*match.version || !*match.file)
		return match_init();

	return match;
}

int count_matches(match_data *matches)
{
	if (!matches) 
	{
		scanlog("Match metadata is empty\n");
		return 0;
	}
	int c = 0;
	for (int i = 0; i < scan_limit && *matches[i].component; i++) c++;
	return c;
}

/* Adds match to matches */
void add_match(match_data match, int total_matches, match_data *matches, bool component_match)
{

	/* Verify if metadata is complete */
	if (!*match.vendor || !*match.component || !*match.url || !*match.version || !*match.file)
	{
		scanlog("Metadata is incomplete: %s,%s,%s,%s,%s\n",match.vendor,match.component,match.version,match.url,match.file);
		return;
	}

	int n = count_matches(matches);

	/* Attempt to place match among existing ones */
	bool placed = false;

	for (int i = 0; i < n; i++)
	{
		/* Are vendor/component the same? */
		if (!strcmp(matches[i].vendor, match.vendor) &&
				!strcmp(matches[i].component, match.component))
		{
			placed = true;

			/* Compare version and, if needed, update range (version-latest) */
			if (strcmp(match.version, matches[i].version) < 0)
			{
				strcpy(matches[i].version, match.version);
			}
			if (strcmp(match.version, matches[i].latest_version) > 0)
			{
				strcpy(matches[i].latest_version, match.version);
			}
		}
	}

	/* Otherwise add a new match */
	if (!placed)
	{
		for (int n = 0; n < scan_limit; n++)
		{
			if (matches[n].path_ln > match.path_ln || !matches[n].path_ln)
			{
				strcpy(matches[n].vendor, match.vendor);
				strcpy(matches[n].component, match.component);
				strcpy(matches[n].version, match.version);
				strcpy(matches[n].latest_version, match.latest_version);
				strcpy(matches[n].url, match.url);
				strcpy(matches[n].file, match.file);
				memcpy(matches[n].component_md5, match.component_md5, MD5_LEN);
				memcpy(matches[n].file_md5, match.file_md5, MD5_LEN);
				matches[n].path_ln = match.path_ln;
				matches[n].selected = match.selected;
				break;
			}
		}
	}
}

/* Returns true if rec_ln is longer than everything else in "matches" */
bool longer_path_in_set(match_data *matches, int total_matches, int rec_ln)
{
	if (scan_limit > total_matches) return false;

	int max_ln = 0;

	for (int i = 0; i < total_matches; i++)
	{
		if (matches[i].path_ln > max_ln) max_ln = matches[i].path_ln;
	}

	return (rec_ln > max_ln);
}

bool handle_component_record(uint8_t *key, uint8_t *subkey, int subkey_ln, uint8_t *raw_data, uint32_t datalen, int iteration, void *ptr)
{
	if (!datalen && datalen >= MAX_PATH) return false;

	uint8_t data[MAX_PATH] = "\0";
	memcpy(data, raw_data, datalen);
	data[datalen] = 0;

	match_data *matches = (match_data*) ptr;
	struct match_data match = match_init();

	/* Exit if we have enough matches */
	int total_matches = count_matches(matches);
	if (total_matches >= scan_limit) return true;

	match = fill_match(NULL, data);

	/* Save match component id */
	memcpy(match.component_md5, key, LDB_KEY_LN);
	memcpy(match.component_md5 + LDB_KEY_LN, subkey, subkey_ln);
	memcpy(match.file_md5, match.component_md5, MD5_LEN);

	add_match(match, total_matches, matches, true);

	return false;
}

/* Determine if a file is to be skipped based on extension or path content */
bool skip_file_path(char *path, match_data *matches)
{
	bool unwanted = false;

	/* Skip blacklisted path */
	if (unwanted_path(path))
	{
		scanlog("Unwanted path\n");
		unwanted = true;
	}

	/* Skip blacklisted extension */
	else if (extension(path) && blacklisted_extension(path))
	{
		scanlog("Blacklisted extension\n");
		unwanted = true;
	}

	/* Compare extension of matched file with scanned file */
	else if (match_extensions)
	{
		char *oss_ext = extension(path);
		char *my_ext = extension(matches->scandata->file_path);
		if (oss_ext) if (my_ext) if (strcmp(oss_ext, my_ext))
		{
			scanlog("Matched file extension does not match source\n");
			unwanted = true;
		}
	}

	if (unwanted) scanlog("Unwanted path %s\n", path);
	return unwanted;
}

bool handle_file_record(uint8_t *key, uint8_t *subkey, int subkey_ln, uint8_t *raw_data, uint32_t datalen, int iteration, void *ptr)
{
	if (!datalen || datalen >= MAX_PATH) return false;

	uint8_t data[MAX_PATH] = "\0";
	memcpy(data, raw_data, datalen);
	data[datalen] = 0;

	/* Save pointer to path in data */
	char *path = (char *) data + MD5_LEN;
	scanlog("Analysing %s\n", path);

	match_data *matches = (match_data*) ptr;

	/* Skip unwanted paths */
	if (skip_file_path(path, matches)) return false;

	struct match_data match = match_init();

	int total_matches = count_matches(matches);

	/* If we have a full set, and this path is longer than others, skip it*/
	if (longer_path_in_set(matches, total_matches, datalen))
	{
		scanlog("Discarding in favour of a shorter path\n");
		return false;
	}

	/* Check if matched file is a blacklisted extension */
	if (extension(path))
		if (blacklisted_extension(path))
		{
			scanlog("Blacklisted extension\n");
			return false;
		}

	/* If component does not exist (orphan file) skip it */
	if (!ldb_key_exists(oss_component, data))
	{
		scanlog("Orphan file\n");
		return false;
	}

	uint8_t *component = calloc(LDB_MAX_REC_LN, 1);
	get_component_record(data, component);
	if (*component)
	{
		match = fill_match(data, component);

		/* Save match file id */
		memcpy(match.file_md5, key, LDB_KEY_LN);
		memcpy(match.file_md5 + LDB_KEY_LN, subkey, subkey_ln);
	}
	else scanlog("No component data found\n");

	add_match(match, total_matches, matches, false);
	free(component);

	return false;
}

struct match_data *prefill_match(scan_data *scan, char *lines, char *oss_lines, int matched_percent)
{
	struct match_data *matches = calloc(sizeof(match_data), scan_limit);
	if (matched_percent > 100) matched_percent = 100;
	for (int i = 0; i < scan_limit; i++)
	{
		matches[i].type = scan->match_type;
		strcpy(matches[i].lines,lines);
		strcpy(matches[i].oss_lines, oss_lines);
		sprintf(matches[i].matched,"%u%%", matched_percent);
		matches[i].selected = false;
		matches[i].scandata = scan;
	}
	return matches;
}

match_data *load_matches(scan_data *scan, uint8_t *matching_md5)
{
	match_data match;

	/* Compile line ranges */
	char *oss_ranges = malloc(sizeof(match.lines)-1);
	strcpy(oss_ranges, "all");
	char *line_ranges = malloc(sizeof(match.lines)-1);
	strcpy(line_ranges, "all");

	/* Compile match ranges and fill up matched percent */
	int hits = 100;
	int matched_percent = 100;

	if (scan->match_type == snippet)
	{
		scanlog("%d hits before compiling ranges\n", hits);
		hits = compile_ranges(matching_md5, line_ranges, oss_ranges);
		float percent = (hits * 100) / scan->total_lines;
		if (hits) matched_percent = floor(percent);

		scanlog("%d hits left after compiling ranges\n", hits);
		if (!hits)
		{
			free(line_ranges);
			free(oss_ranges);
			return NULL;
		}
	}

	struct match_data *matches = prefill_match(scan, line_ranges, oss_ranges, matched_percent);
	free(oss_ranges);
	free(line_ranges);

	uint32_t records = 0;

	/* Snippet and component match should look for the matching_md5 in components */
	if (scan->match_type != file)
	{
		records = ldb_fetch_recordset(NULL, oss_component, matching_md5, false, handle_component_record, (void *) matches);
		scanlog("Component recordset contains %u records\n", records);
	}

	if (!records)
	{
		records = ldb_fetch_recordset(NULL, oss_file, matching_md5, false, handle_file_record, (void *) matches);
		scanlog("File recordset contains %u records\n", records);
	}

	if (records) return matches;

	if (matches) free(matches);

	scanlog("Match type is 'none' after loading matches\n");
	return NULL;
}

match_data *compile_matches(scan_data *scan)
{
	uint8_t *matching_md5 = scan->md5;

	/* Search for biggest snippet */
	if (scan->match_type == snippet)
	{
		matching_md5 = biggest_snippet(scan);
		scanlog("%ld matches in snippet map\n", scan->matchmap_size);
	}

	/* Return NULL if no matches */
	if (!matching_md5)
	{
		scan->match_type = none;
		scanlog("No matching file id\n");
		return NULL;
	}
	else
	{
		/* Log matching MD5 */
		for (int i = 0; i < MD5_LEN; i++) scanlog("%02x", matching_md5[i]);
		scanlog(" selected\n");
	}

	/* Dump match map */
	if (debug_on) map_dump(scan);

	/* Gather and load match metadata */
	match_data *matches = NULL;

	scanlog("Starting match: %s\n",matchtypes[scan->match_type]);
	if (scan->match_type != none) matches = load_matches(scan, matching_md5);

	/* The latter could result in no matches */
	if (!matches) scan->match_type = none;
	scanlog("Final match: %s\n",matchtypes[scan->match_type]);

	return matches;
}

/* Scans a wfp file with winnowing fingerprints */
int wfp_scan(scan_data *scan)
{
	char * line = NULL;
	size_t len = 0;
	ssize_t lineln;
	uint8_t *rec = calloc(LDB_MAX_REC_LN, 1);
	scan->preload = true;

	/* Open WFP file */
	FILE *fp = fopen(scan->file_path, "r");
	if (fp == NULL)
	{
		fprintf(stdout, "E017 Cannot open target");
		return EXIT_FAILURE;
	}
	bool read_data = false;

	/* Read line by line */
	while ((lineln = getline(&line, &len, fp)) != -1)
	{
		trim(line);

		bool is_component = (memcmp(line, "component=", 4) == 0);
		bool is_file = (memcmp(line, "file=", 5) == 0);
		bool is_wfp = (!is_file && !is_component);

		/* Scan previous file */
		if ((is_component || is_file) && read_data) ldb_scan(scan);

		/* Parse file information with format: file=MD5(32),file_size,file_path */
		if (is_file)
		{
			scan_data_reset(scan);
			const int tagln = 5; // len of 'file='

			/* Get file MD5 */
			char *hexmd5 = calloc(MD5_LEN * 2 + 1, 1);
			memcpy(hexmd5, line + tagln, MD5_LEN * 2);
			hex_to_bin(hexmd5, MD5_LEN * 2, scan->md5);
			free(hexmd5);

			/* Extract fields from file record */
			strcpy((char *)rec, line + tagln + (MD5_LEN * 2) + 1);
			extract_csv(scan->file_size, (char *)rec, 1, LDB_MAX_REC_LN);
			extract_csv(scan->file_path, (char *)rec, 2, LDB_MAX_REC_LN);

			read_data = true;
		}

		/* Save hash/es to memory. Parse file information with format:
		   linenr=wfp(6)[,wfp(6)]+ */

		if (is_wfp && (scan->hash_count < MAX_HASHES_READ))
		{
			/* Split string by the equal and commas */
			int line_ln = strlen(line);
			for (int e = 0; e < line_ln; e++) if (line[e]=='=' || line[e]==',') line[e] = 0;

			/* Extract line number */
			int line_nr = atoi(line);

			/* Move pointer to the first hash */
			char *hexhash = line + strlen(line) + 1;

			/* Save all hashes in the present line */
			while (*hexhash) {

				/* Convert hash to binary */
				hex_to_bin(hexhash, 8, (uint8_t *)&scan->hashes[scan->hash_count]);
				uint32_reverse((uint8_t *)&scan->hashes[scan->hash_count]);

				/* Save line number */
				scan->lines[scan->hash_count] = line_nr;

				/* Move pointer to the next hash */
				hexhash += strlen(hexhash) + 1;

				scan->hash_count++;
			}
		}
	}

	/* Scan the last file */
	if (read_data) ldb_scan(scan);

	fclose(fp);
	if (line) free(line);
	free(rec);

	return EXIT_SUCCESS;
}

/* Scans a file and returns JSON matches via STDOUT
   scan structure can be already preloaded (.wfp scan)
   otherwise, it will be loaded here (scanning a physical file) */
bool ldb_scan(scan_data *scan)
{
	bool skip = false;

	scan->matchmap_size = 0;
	scan->match_type = none;
	scan->timer = microseconds_now();

	/* Get file length */
	uint64_t file_size;
	if (scan->preload) file_size = atoi(scan->file_size);
	else file_size = get_file_size(scan->file_path);

	/* Error reading file */
	if (file_size < 0) ldb_error("Cannot access file");

	/* Calculate MD5 hash (if not already preloaded) */
	if (!scan->preload) file_md5(scan->file_path, scan->md5);

	if (extension(scan->file_path))
		if (blacklisted_extension(scan->file_path)) skip = true;

	/* Ignore <=1 byte */
	if (file_size <= 1) skip = true;

	if (!skip)
	{
		/* Scan full file */
		scan->match_type = ldb_scan_file(scan->md5);

		/* If no match, scan snippets */
		if (scan->match_type == none)
		{
			/* Load snippets into scan data */
			if (!scan->preload)
			{
				/* Read file into memory */
				char *src = calloc(MAX_FILE_SIZE, 1);
				if (file_size < MAX_FILE_SIZE) read_file(src, scan->file_path, 0);

				/* Determine if file is to skip snippet search */
				if (!skip_snippets(src, file_size))
				{
					/* Load wfps into scan structure */
					scan->hash_count = winnowing(src, scan->hashes, scan->lines, MAX_FILE_SIZE);
					if (scan->hash_count) scan->total_lines = scan->lines[scan->hash_count - 1];
				}
				free(src);
			}
			else if (scan->hash_count) scan->total_lines = scan->lines[scan->hash_count - 1];

			/* Perform snippet scan */
			if (scan->total_lines) scan->match_type = ldb_scan_snippets(scan);

			else scanlog("File skipped\n");
		}
	}

	/* Compile matches */
	match_data *matches = compile_matches(scan);
	int total_matches = count_matches(matches);

	/* Debug match info */
	scanlog("%d matches compiled\n", total_matches);
	if (debug_on) for (int i = 0; i < total_matches; i++)
		scanlog("%d, %s\n", matches[i].path_ln, matches[i].file);

	/* Matched asset in SBOM.json? */
	for (int i = 0; i < total_matches; i++)
	{
		if (assets_match(matches[i]))
		{
			scanlog("Asset matched\n");
			if (matches) free(matches);
			matches = NULL;
			scan->match_type = none;
			break;
		}
	}

	/* Perform post scan intelligence */
	if (scan->match_type != none)
	{
		scanlog("Starting post-scan analysis\n");
		post_scan(matches);
	}

	/* Output matches */
	scanlog("Match output starts\n");
	output_matches_json(matches, scan);

	if (matches) free(matches);
	scan_data_reset(scan);
	return true;
}
