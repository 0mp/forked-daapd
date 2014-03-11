
#ifndef __FILESCANNER_H__
#define __FILESCANNER_H__

#include "db.h"

#define F_SCAN_TYPE_FILE         (1 << 0)
#define F_SCAN_TYPE_PODCAST      (F_SCAN_TYPE_FILE | (1 << 1))
#define F_SCAN_TYPE_AUDIOBOOK    (F_SCAN_TYPE_FILE | (1 << 2))
#define F_SCAN_TYPE_COMPILATION  (F_SCAN_TYPE_FILE | (1 << 3))
#define F_SCAN_TYPE_URL          (1 << 4)
#define F_SCAN_TYPE_SPOTIFY      (1 << 5)

int
filescanner_init(void);

void
filescanner_deinit(void);

int
filescanner_status(void);

void
filescanner_process_media(char *path, time_t mtime, off_t size, int type, struct media_file_info *external_mfi);

/* Actual scanners */
int
scan_metadata_ffmpeg(char *file, struct media_file_info *mfi);

int
scan_metadata_icy(char *url, struct media_file_info *mfi);

void
scan_m3u_playlist(char *file, time_t mtime);

#ifdef ITUNES
void
scan_itunes_itml(char *file);
#endif

#endif /* !__FILESCANNER_H__ */
