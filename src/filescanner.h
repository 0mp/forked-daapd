
#ifndef __FILESCANNER_H__
#define __FILESCANNER_H__

#include "ff-dbstruct.h"

int
filescanner_init(void);

void
filescanner_deinit(void);

/* Actual scanners */
int
scan_get_ffmpeginfo(char *filename, struct media_file_info *mfi);

int
scan_get_urlinfo(char *filename, struct media_file_info *mfi);

int
scan_static_playlist(char *filename);

int
scan_m3u_playlist(char *file);

#endif /* !__FILESCANNER_H__ */
