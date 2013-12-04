#ifndef CLEANSPOOL_H
#define CLEANSPOOL_H

void *clean_spooldir_run(void *dummy);
void check_spooldir_filesindex(const char *path = NULL, const char *dirfilter = NULL);
void convert_filesindex();
bool check_exists_act_records_in_files();
void clean_obsolete_dirs(const char *path = NULL);
bool isSetCleanspoolParameters();

#endif
