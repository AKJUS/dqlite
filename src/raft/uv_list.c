#include <string.h>
#include <uv.h>

#include "assert.h"
#include "uv.h"

static const char *uvListIgnored[] = {".", "..", "metadata1", "metadata2",
				      NULL};

/* Return true if the given filename should be ignored. */
static bool uvListShouldIgnore(const char *filename)
{
	const char **cursor = uvListIgnored;
	bool result = false;
	if (strlen(filename) >= UV__FILENAME_LEN) {
		return true;
	}
	while (*cursor != NULL) {
		if (strcmp(filename, *cursor) == 0) {
			result = true;
			break;
		}
		cursor++;
	}
	return result;
}

int UvList(struct uv *uv,
	   struct uvSnapshotInfo *snapshots[],
	   size_t *n_snapshots,
	   struct uvSegmentInfo *segments[],
	   size_t *n_segments,
	   char *errmsg)
{
	struct uv_fs_s req;
	struct uv_dirent_s entry;
	int n;
	int i;
	int rv;

	n = uv_fs_scandir(NULL, &req, uv->dir, 0, NULL);
	if (n < 0) {
		ErrMsgPrintf(errmsg, "scan data directory: %s", uv_strerror(n));
		return RAFT_IOERR;
	}

	*snapshots = NULL;
	*n_snapshots = 0;

	*segments = NULL;
	*n_segments = 0;

	rv = 0;

	for (i = 0; i < n; i++) {
		const char *filename;
		bool appended;

		rv = uv_fs_scandir_next(&req, &entry);
		assert(rv == 0); /* Can't fail in libuv */

		filename = entry.name;

		/* If an error occurred while processing a preceeding entry or
		 * if we know that this is not a segment filename, just free it
		 * and skip to the next one. */
		if (uvListShouldIgnore(filename)) {
			tracef("ignore %s", filename);
			continue;
		}

		/* Append to the snapshot list if it's a snapshot metadata
		 * filename and a valid associated snapshot file exists. */
		rv = UvSnapshotInfoAppendIfMatch(uv, filename, snapshots,
						 n_snapshots, &appended);
		if (rv != 0) {
			goto error;
		}
		if (appended) {
			tracef("snapshot %s", filename);
			continue;
		}

		/* Append to the segment list if it's a segment filename */
		rv = uvSegmentInfoAppendIfMatch(entry.name, segments,
						n_segments, &appended);
		if (rv != 0) {
			goto error;
		}
		if (appended) {
			tracef("segment %s", filename);
			continue;
		}

		tracef("ignore %s", filename);
	}
	rv = uv_fs_scandir_next(&req, &entry);
	assert(rv == UV_EOF);

	if (*snapshots != NULL) {
		UvSnapshotSort(*snapshots, *n_snapshots);
	}
	if (*segments != NULL) {
		uvSegmentSort(*segments, *n_segments);
	}
	return 0;

error:
	uv_fs_req_cleanup(&req);
	raft_free(*segments);
	*segments = NULL;
	raft_free(*snapshots);
	*snapshots = NULL;
	return rv;
}

