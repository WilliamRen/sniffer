#include <regex.h>
#include <sys/time.h>
#include <string.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <net/if.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/statvfs.h>
#include <curl/curl.h>
#include <cerrno>
#include <json.h>
#include <iomanip>
#include <openssl/sha.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>

#include "voipmonitor.h"

#ifdef FREEBSD
#include <sys/uio.h>
#include <sys/thr.h>
#else
#include <sys/sendfile.h>
#endif

#include <algorithm> // for std::min
#include <iostream>

#include "tools_dynamic_buffer.h"
#include "voipmonitor.h"
#include "calltable.h"
#include "rtp.h"
#include "tools.h"
#include "md5.h"
#include "pcap_queue.h"
#include "sql_db.h"
#include "tar.h"
#include "tools.h"
#include "config.h"
#include "cleanspool.h"


using namespace std;

map<unsigned int, int> tartimemap;
pthread_mutex_t tartimemaplock;

extern char opt_chdir[1024];     
volatile unsigned int glob_tar_queued_files;

extern int opt_pcap_dump_tar_compress_sip; //0 off, 1 gzip, 2 lzma
extern int opt_pcap_dump_tar_sip_level;
extern int opt_pcap_dump_tar_compress_rtp;
extern int opt_pcap_dump_tar_rtp_level;
extern int opt_pcap_dump_tar_compress_graph;
extern int opt_pcap_dump_tar_graph_level;
extern int opt_pcap_dump_tar_threads;

extern int opt_filesclean;
extern int opt_nocdr;

extern MySqlStore *sqlStore;


extern int terminating; 
extern TarQueue *tarQueue;
extern volatile unsigned int glob_last_packet_time;


map<void*, unsigned int> okTarPointers;
volatile int _sync_okTarPointers;
inline void lock_okTarPointers() { while(__sync_lock_test_and_set(&_sync_okTarPointers, 1)); }
inline void unlock_okTarPointers() { __sync_lock_release(&_sync_okTarPointers); }


/* magic, version, and checksum */
void
Tar::th_finish()
{
	int i, sum = 0;

	strncpy(tar.th_buf.magic, "ustar  ", 8);

	for (i = 0; i < T_BLOCKSIZE; i++)
		sum += ((char *)(&(tar.th_buf)))[i];
	for (i = 0; i < 8; i++)
		sum += (' ' - tar.th_buf.chksum[i]);
	int_to_oct(sum, tar.th_buf.chksum, 8);
}

/* encode file path */
void
Tar::th_set_path(char *pathname, bool partSuffix)
{
#ifdef DEBUG
	printf("in th_set_path(th, pathname=\"%s\")\n", pathname);
#endif  

	if (tar.th_buf.gnu_longname != NULL)
		free(tar.th_buf.gnu_longname);
	tar.th_buf.gnu_longname = NULL;
	
	/* classic tar format */
	
	snprintf(tar.th_buf.name, 100, "%s", pathname);
	
	map<string, u_int32_t>::iterator it = partCounter.find(pathname);
	if(it == partCounter.end()) {
		partCounter[pathname] = 1;
		++partCounterSize;
	} else {
		++partCounter[pathname];
	}
	if(partSuffix) {
		snprintf(tar.th_buf.name + strlen(tar.th_buf.name), 100 - strlen(tar.th_buf.name), "_%u", partCounter[pathname]);
	}
	       
#ifdef DEBUG   
	puts("returning from th_set_path()...");
#endif 
}

/* map a file mode to a typeflag */
void
Tar::th_set_type(mode_t mode)
{       
	tar.th_buf.typeflag = 0; // regular file
}


/* encode device info */
void
Tar::th_set_device(dev_t device)
{
#ifdef DEBUG
	printf("th_set_device(): major = %d, minor = %d\n",
	       major(device), minor(device));
#endif 
	int_to_oct(major(device), tar.th_buf.devmajor, 8);
	int_to_oct(minor(device), tar.th_buf.devminor, 8);
}


/* encode user info */
void
Tar::th_set_user(uid_t uid)
{
	struct passwd *pw;

	pw = getpwuid(uid);
	if (pw != NULL)
		*((char *)mempcpy(tar.th_buf.uname, pw->pw_name, sizeof(tar.th_buf.uname))) = '\0';

	int_to_oct(uid, tar.th_buf.uid, 8);
}


/* encode group info */
void
Tar::th_set_group(gid_t gid)
{

/*
	struct group *gr;

	gr = getgrgid(gid);
	if (gr != NULL)
		*((char *)mempcpy(tar.th_buf.gname, gr->gr_name, sizeof(tar.th_buf.gname))) = '\0';
*/

	int_to_oct(gid, tar.th_buf.gid, 8);
}


/* encode file mode */
void
Tar::th_set_mode( mode_t fmode)
{      
	int_to_oct(fmode, tar.th_buf.mode, 8);
}


/* calculate header checksum */
int    
Tar::th_crc_calc()
{
	int i, sum = 0;

	for (i = 0; i < T_BLOCKSIZE; i++)
		sum += ((unsigned char *)(&(tar.th_buf)))[i];
	for (i = 0; i < 8; i++)
		sum += (' ' - (unsigned char)tar.th_buf.chksum[i]);
       
	return sum;
}      

	       
/* string-octal to integer conversion */
int
Tar::oct_to_int(char *oct)
{
	int i;
       
	sscanf(oct, "%o", &i);

	return i;
}


/* integer to string-octal conversion, no NULL */
void   
Tar::int_to_oct_nonull(int num, char *oct, size_t octlen)
{      
	snprintf(oct, (unsigned long)octlen, "%*lo", (int)octlen - 1, (unsigned long)num);
	oct[octlen - 1] = ' ';
}

int
Tar::tar_init(int oflags, int mode, int options)
{
	memset(&tar, 0, sizeof(TAR));
	
	tar.options = options;
//	tar.type = (type ? type : &default_type);
	tar.oflags = oflags;

/*
	if ((oflags & O_ACCMODE) == O_RDONLY)
		tar.h = libtar_hash_new(256, (libtar_hashfunc_t)path_hashfunc);
	else
		tar.h = libtar_hash_new(16, (libtar_hashfunc_t)dev_hash);
*/
	return 0;
}

/* open a new tarfile handle */
int     
Tar::tar_open(string pathname, int oflags, int mode, int options)
{       
	this->pathname = pathname;
	if (tar_init(oflags, mode, options) == -1)
		return -1;

	if ((options & TAR_NOOVERWRITE) && (oflags & O_CREAT))
		oflags |= O_EXCL;

	if(file_exists(pathname)) {
		int i = 1;
		while(i < 100) {
			stringstream newpathname;
			newpathname << this->pathname;
			newpathname << "." << i;
			if(file_exists(newpathname.str())) {
				++i;
				continue;
			} else {
				rename(pathname.c_str(), newpathname.str().c_str());
				if(sverb.tar) syslog(LOG_NOTICE, "tar: renaming %s -> %s", pathname.c_str(), newpathname.str().c_str());
				break;
			}
		}
	}
	tar.fd = open((char*)this->pathname.c_str(), oflags, mode);
	if (tar.fd == -1)
	{
		return -1;
	}
	return 0;
}

/* write a header block */
int
Tar::th_write()
{
	int i;
	th_finish();
	i = tar_block_write((const char*)&(tar.th_buf), T_BLOCKSIZE);
	if (i != T_BLOCKSIZE)
	{	       
//		if (i != -1)    
//			errno = EINVAL;
		return -1;
	}
		
#ifdef DEBUG    
	puts("th_write(): returning 0");
#endif	       
	return 0;      
}		       

/* add file contents to a tarchive */
int
Tar::tar_append_buffer(ChunkBuffer *buffer, size_t lenForProceed)
{
	buffer->chunkIterate(this, true, true, lenForProceed);
	return 0;
}

void 
Tar::chunkbuffer_iterate_ev(char *data, u_int32_t len, u_int32_t pos) {
	if(data) {
		tar_block_write(data, len);
	} else if(pos % T_BLOCKSIZE) {
		char zeroblock[T_BLOCKSIZE];
		memset(zeroblock, 0, T_BLOCKSIZE - pos % T_BLOCKSIZE);
		tar_block_write(zeroblock, T_BLOCKSIZE - pos % T_BLOCKSIZE);
	}
}

int    
Tar::initZip() {
	if(!this->zipStream) {
		this->zipStream =  new z_stream;
		this->zipStream->zalloc = Z_NULL;
		this->zipStream->zfree = Z_NULL;
		this->zipStream->opaque = Z_NULL;
		if(deflateInit2(this->zipStream, gziplevel, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
			deflateEnd(this->zipStream);
			//this->setError("zip initialize failed");
			return(false);
		} else {
			this->zipBufferLength = 8192*4;
			this->zipBuffer = new char[this->zipBufferLength];
		}
	}
	return(true);
}
       
void 
Tar::flushZip() {
	do {
		this->zipStream->avail_out = this->zipBufferLength;
		this->zipStream->next_out = (unsigned char*)this->zipBuffer;
		if(deflate(this->zipStream, Z_FINISH)) {
			int have = this->zipBufferLength - this->zipStream->avail_out;
			if(::write(tar.fd, (const char*)this->zipBuffer, have) <= 0) {
				//this->setError();
				break;
			};
		}
	} while(this->zipStream->avail_out == 0);
}	

ssize_t
Tar::writeZip(const void *buf, size_t len) {
	int flush = 0;
	if(!this->initZip()) {
		return(false);
	}      
	this->zipStream->avail_in = len;
	this->zipStream->next_in = (unsigned char*)buf;
	do {
		this->zipStream->avail_out = this->zipBufferLength;
		this->zipStream->next_out = (unsigned char*)this->zipBuffer;

		if(deflate(this->zipStream, flush ? Z_FINISH : Z_NO_FLUSH) != Z_STREAM_ERROR) {
			int have = this->zipBufferLength - this->zipStream->avail_out;
			if(::write(tar.fd, (const char*)this->zipBuffer, have) <= 0) {
				//this->setError();
				return(false);
			};     
		} else {
			//this->setError("zip deflate failed");
			return(false);
		}      
	} while(this->zipStream->avail_out == 0);
	return(true);
}      

#ifdef HAVE_LIBLZMA
int
Tar::initLzma() {
	if(!this->lzmaStream) {
		/* initialize xz encoder */
		//uint32_t preset = LZMA_COMPRESSION_LEVEL | (LZMA_COMPRESSION_EXTREME ? LZMA_PRESET_EXTREME : 0);
		lzmaStream = new lzma_stream;
		*lzmaStream = LZMA_STREAM_INIT; 

		int ret_xz = lzma_easy_encoder (this->lzmaStream, lzmalevel, LZMA_CHECK_CRC64);
		if (ret_xz != LZMA_OK) {
			fprintf (stderr, "lzma_easy_encoder error: %d\n", (int) ret_xz);
			return(false);
		}
		if(!zipBuffer) {
			this->zipBufferLength = 8192*4;
			this->zipBuffer = new char[this->zipBufferLength];
		}
	}
	return(true);
}

void 
Tar::flushLzma() {
	int ret_xz;
//	this->lzmaStream->next_in = NULL;
//	this->lzmaStream->avail_in = 0;
	do {
		this->lzmaStream->avail_out = this->zipBufferLength;
		this->lzmaStream->next_out = (unsigned char*)this->zipBuffer;
		ret_xz = lzma_code(this->lzmaStream, LZMA_FINISH);
		if(ret_xz == LZMA_STREAM_END) {
			int have = this->zipBufferLength - this->lzmaStream->avail_out;
			if(::write(tar.fd, (const char*)this->zipBuffer, have) <= 0) {
				//this->setError();
				break;
			};
			break;
		}
		int have = this->zipBufferLength - this->lzmaStream->avail_out;
		if(::write(tar.fd, (const char*)this->zipBuffer, have) <= 0) {
			//this->setError();
			break;
		};
	} while(1);
}	

ssize_t
Tar::writeLzma(const void *buf, size_t len) {
	int ret_xz;
	if(!this->initLzma()) {
		return(false);
	}
	this->lzmaStream->next_in = (const uint8_t*)buf;
	this->lzmaStream->avail_in = len;
	do {
		this->lzmaStream->next_out = (unsigned char*)this->zipBuffer;
		this->lzmaStream->avail_out = this->zipBufferLength;

		/* compress data */
		ret_xz = lzma_code(lzmaStream, LZMA_RUN);

		if ((ret_xz != LZMA_OK) && (ret_xz != LZMA_STREAM_END)) {
			fprintf (stderr, "lzma_code error: %d\n", (int) ret_xz);
			return LZMA_RET_ERROR_COMPRESSION;
		} else {
			int have = this->zipBufferLength - this->lzmaStream->avail_out;
			if(::write(tar.fd, (const char*)this->zipBuffer, have) <= 0) {
				//this->setError();
				return(false);
			}
		}
	} while(this->lzmaStream->avail_out == 0);
	return(true);
}      
#endif

int
Tar::tar_block_write(const char *buf, u_int32_t len){
	int zip = false;
	int lzma = false;
	switch(tar.qtype) {
	case 1:
		if(opt_pcap_dump_tar_compress_sip == 1) {
			gziplevel = opt_pcap_dump_tar_sip_level;
			zip = true;
		} else if(opt_pcap_dump_tar_compress_sip == 2) {
			lzmalevel = opt_pcap_dump_tar_sip_level;
			lzma = true;
		}
		break;
	case 2:
		if(opt_pcap_dump_tar_compress_rtp == 1) {
			gziplevel = opt_pcap_dump_tar_rtp_level;
			zip = true;
		} else if(opt_pcap_dump_tar_compress_rtp == 2) {
			lzmalevel = opt_pcap_dump_tar_rtp_level;
			lzma = true;
		}
		break;
	case 3:
		if(opt_pcap_dump_tar_compress_graph == 1) {
			gziplevel = opt_pcap_dump_tar_graph_level;
			zip = true;
		} else if(opt_pcap_dump_tar_compress_graph == 2) {
			lzmalevel = opt_pcap_dump_tar_graph_level;
			lzma = true;
		}
		break;
	}
	
	if(zip) {
		writeZip((char *)(buf), len);
	} else if(lzma){
		#ifdef HAVE_LIBLZMA
		writeLzma((char *)(buf), len);
		#endif //HAVE_LIBLZMA
	} else {
		::write(tar.fd, (char *)(buf), len);
	}
	
	
	return(len);
};

void Tar::tar_close() {
	char zeroblock[T_BLOCKSIZE];
	memset(zeroblock, 0, T_BLOCKSIZE);
	tar_block_write(zeroblock, T_BLOCKSIZE);
	tar_block_write(zeroblock, T_BLOCKSIZE);
	if(this->zipStream) {
		flushZip();
		deflateEnd(this->zipStream);
		delete this->zipStream;
	}
#if HAVE_LIBLZMA
	if(this->lzmaStream) {
		flushLzma();
		lzma_end(this->lzmaStream);
		delete this->lzmaStream;
		this->lzmaStream = NULL;
	}
#endif
	if(this->zipBuffer) {
		delete [] this->zipBuffer;
	}
	addtofilesqueue();
	if(sverb.tar) syslog(LOG_NOTICE, "tar %s destroyd (destructor)\n", pathname.c_str());
}

void Tar::addtofilesqueue() {

	string column;
	switch(tar.qtype) {
	case 1:
		column = "sipsize";
		break;
	case 2:
		column = "rtpsize";
		break;
	case 3:
		column = "graphsize";
		break;
	default:
		column = "rtpsize";
	}

	if(!opt_filesclean or opt_nocdr or !isSqlDriver("mysql") or !isSetCleanspoolParameters()) return;

	long long size = 0;
	size = GetFileSizeDU(pathname.c_str());

	if(size == (long long)-1) {
		//error or file does not exists
		char buf[4092];
		buf[0] = '\0';
		strerror_r(errno, buf, 4092);
		syslog(LOG_ERR, "addtofilesqueue ERROR file[%s] - error[%d][%s]", pathname.c_str(), errno, buf);
		return;
	}

	if(size == 0) {
		// if the file has 0 size we still need to add it to cleaning procedure
		size = 1;
	}

	ostringstream query;

	extern int opt_id_sensor_cleanspool;
	int id_sensor = opt_id_sensor_cleanspool == -1 ? 0 : opt_id_sensor_cleanspool;


/* returns name of the directory in format YYYY-MM-DD */
        char sdirname[12];
        snprintf(sdirname, 11, "%04d%02d%02d%02d",  year, mon, day, hour);
        sdirname[11] = 0;
        string dirnamesqlfiles(sdirname);

	query << "INSERT INTO files SET files.datehour = " << dirnamesqlfiles << ", id_sensor = " << id_sensor << ", "
		<< column << " = " << size << " ON DUPLICATE KEY UPDATE " << column << " = " << column << " + " << size;

	sqlStore->lock(STORE_PROC_ID_CLEANSPOOL);
	sqlStore->query(query.str().c_str(), STORE_PROC_ID_CLEANSPOOL);

	ostringstream fname;
	fname << "filesindex/" << column << "/" << dirnamesqlfiles;
	ofstream myfile(fname.str().c_str(), ios::app | ios::out);
	if(!myfile.is_open()) {
		syslog(LOG_ERR,"error write to [%s]", fname.str().c_str());
	}
	myfile << pathname << ":" << size << "\n";
	myfile.close();    

	sqlStore->unlock(STORE_PROC_ID_CLEANSPOOL);
}

Tar::~Tar() {
	tar_close();
}

void			   
TarQueue::add(string filename, unsigned int time, ChunkBuffer *buffer){
	__sync_add_and_fetch(&glob_tar_queued_files, 1);
	data_t data;
	data.buffer = buffer;
	lock();
	unsigned int year, mon, day, hour, minute;
	char type[12];
	char fbasename[2*1024];
	sscanf(filename.c_str(), "%u-%u-%u/%u/%u/%[^/]/%s", &year, &mon, &day, &hour, &minute, type, fbasename);
//      printf("%s: %u-%u-%u/%u/%u/%s/%s\n", filename.c_str(), year, mon, day, hour, minute, type, fbasename);
	data.filename = fbasename;
	data.year = year;
	data.mon = mon;
	data.day = day;
	data.hour = hour;
	data.minute = minute;
	if(type[0] == 'S') {
		queue[1][time - time % TAR_MODULO_SECONDS].push_back(data);
	} else if(type[0] == 'R') {
		queue[2][time - time % TAR_MODULO_SECONDS].push_back(data);
	} else if(type[0] == 'G') {
		queue[3][time - time % TAR_MODULO_SECONDS].push_back(data);
	}      
//	if(sverb.tar) syslog(LOG_NOTICE, "adding tar %s len:%u\n", filename.c_str(), buffer->len);

	unlock();
}      

string
qtype2str(int qtype) {
	if(qtype == 1) return "sip";
	else if(qtype == 2) return "rtp";
	else if(qtype == 3) return "graph";
	else return "all";
}

string
qtype2strC(int qtype) {
	if(qtype == 1) return "SIP";
	else if(qtype == 2) return "RTP";
	else if(qtype == 3) return "GRAPH";
	else return "ALL";
}


void decreaseTartimemap(unsigned int time){
	// decrease tartimemap
	pthread_mutex_lock(&tartimemaplock);
	map<unsigned int, int>::iterator tartimemap_it = tartimemap.find(time - time % TAR_MODULO_SECONDS);
	if(tartimemap_it != tartimemap.end()) {
		tartimemap_it->second--;
		if(sverb.tar > 2) {
			syslog(LOG_NOTICE, "tartimemap decrease to: %i %i %i", 
			       time, time - time % TAR_MODULO_SECONDS, tartimemap_it->second);
		}
		if(tartimemap_it->second == 0){
			tartimemap.erase(tartimemap_it);
		}
	}
	pthread_mutex_unlock(&tartimemaplock);
}


void increaseTartimemap(unsigned int time){
	pthread_mutex_lock(&tartimemaplock);
	map<unsigned int, int>::iterator tartimemap_it = tartimemap.find(time - time % TAR_MODULO_SECONDS);
	if(tartimemap_it != tartimemap.end()) {
		tartimemap_it->second++;
		if(sverb.tar > 2) {
			syslog(LOG_NOTICE, "tartimemap increase to: %i %i %i", 
			       time, time - time % TAR_MODULO_SECONDS, tartimemap_it->second);
		}
	} else {
		tartimemap[time - time % TAR_MODULO_SECONDS] = 1;
		if(sverb.tar > 2) {
			syslog(LOG_NOTICE, "tartimemap increase set: %i %i %i", 
			       time, time - time % TAR_MODULO_SECONDS, 1);
		}
	}
	pthread_mutex_unlock(&tartimemaplock);
}


int			    
TarQueue::write(int qtype, unsigned int time, data_t data) {
	stringstream tar_dir, tar_name;
	tar_dir << opt_chdir << "/" << setfill('0') << setw(4) << data.year << setw(1) << "-" << setw(2) << data.mon << setw(1) << "-" << setw(2) << data.day << setw(1) << "/" << setw(2) << data.hour << setw(1) << "/" << setw(2) << data.minute << setw(1) << "/" << setw(0) << qtype2strC(qtype);
	
	tar_name << tar_dir.str() << "/" << qtype2str(qtype) << "_" << setfill('0') << setw(4) << data.year << setw(1) << "-" << setw(2) << data.mon << setw(1) << "-" << setw(2) << data.day << setw(1) << "-" << setw(2) << data.hour << setw(1) << "-" << setw(2) << data.minute << ".tar";
	switch(qtype) {
	case 1:
		switch(opt_pcap_dump_tar_compress_sip) {
		case 1:
			tar_name << ".gz";
			break;
		case 2:
			tar_name << ".xz";
			break;
		}
		break;
	case 2:
		switch(opt_pcap_dump_tar_compress_rtp) {
		case 1:
			tar_name << ".gz";
			break;
		case 2:
			tar_name << ".xz";
			break;
		}
		break;
	case 3:
		switch(opt_pcap_dump_tar_compress_graph) {
		case 1:
			tar_name << ".gz";
			break;
		case 2:
			tar_name << ".xz";
			break;
		}
		break;
	}
	mkdir_r(tar_dir.str(), 0777);
	//printf("tar_name %s\n", tar_name.str().c_str());
       
	pthread_mutex_lock(&tarslock);
	Tar *tar = tars[tar_name.str()];
	if(!tar) {
		tar = new Tar;
		lock_okTarPointers();
		okTarPointers[tar] = glob_last_packet_time;
		unlock_okTarPointers();
		if(sverb.tar) syslog(LOG_NOTICE, "new tar %s\n", tar_name.str().c_str());
		if(sverb.tar) syslog(LOG_NOTICE, "add tar pointer %lx\n", (long)tar);
		tars[tar_name.str()] = tar;
		pthread_mutex_unlock(&tarslock);
		tar->tar_open(tar_name.str(), O_WRONLY | O_CREAT | O_APPEND, 0777, TAR_GNU);
		tar->tar.qtype = qtype;
		tar->created_at = time;
		tar->year = data.year;
		tar->mon = data.mon;
		tar->day = data.day;
		tar->hour = data.hour;
		tar->minute = data.minute;
		
		if(sverb.tar > 2) {
			char dateTimeString[20];
			sprintf(dateTimeString, "%4i-%02i-%02i %02i:%02i:00",
				data.year, data.mon, data.day, data.hour, data.minute);
			if(dateTimeString != sqlDateTimeString(tar->created_at)) {
				syslog(LOG_ERR, "BAD TAR set created_at: %s %lx %s %s %i %i",
				       tar->pathname.c_str(), (long)tar, dateTimeString, sqlDateTimeString(tar->created_at).c_str(),
				       time, data.buffer->getTime()); 
			}
		}

		// allocate it to thread with the lowest total byte len 
		unsigned long int min = 0 - 1;
		int winner = 0;
		for(int i = maxthreads - 1; i >= 0; i--) {
			size_t _len = tarthreads[i].getLen();
			if(min >= _len) {
				min = _len;
				winner = i;
			}
		}
		tar->thread_id = winner;
	} else {
		pthread_mutex_unlock(&tarslock);
	}
     
	data.tar = tar;
	data.time = time;
	pthread_mutex_lock(&tarthreads[tar->thread_id].queuelock);
//	printf("push id:%u\n", tar->thread_id);
	tarthreads[tar->thread_id].queue.push_back(data);
	pthread_mutex_unlock(&tarthreads[tar->thread_id].queuelock);
	return 0;
}

void *TarQueue::tarthreadworker(void *arg) {

	u_int32_t tarChunk_kB = 128;
 
	TarQueue *this2 = ((tarthreadworker_arg*)arg)->tq;
	tarthreads_t *tarthread = &this2->tarthreads[((tarthreadworker_arg*)arg)->i];
	delete (tarthreadworker_arg*)arg;

	tarthread->threadId = get_unix_tid();

	while(1) {
		while(1) {
			data_t data;
			bool findData = false;
			bool isClosed = false;
			size_t lenForProceed = 0;
			size_t lenForProceedSafe = 0;
			pthread_mutex_lock(&tarthread->queuelock);
			if(tarthread->queue.empty()) { 
				if(this2->terminate) {
					pthread_mutex_unlock(&tarthread->queuelock);
					return NULL;
				}
			} else {
				list<data_t>::iterator it;
				for(it = tarthread->queue.begin(); it != tarthread->queue.end(); ) {
					if(!it->buffer) {
						it++;
						continue;
					}
					if(it->buffer->isDecompressError()) {
						tarthread->queue.erase(it++);
						continue;
					}
					lock_okTarPointers();
					if(okTarPointers.find(it->tar) == okTarPointers.end()) {
						data = *it; // only for debugging
						if(sverb.tar > 1) {
							syslog(LOG_ERR, "BAD TAR POINTER %lx %s %s\n", (long)it->tar, data.buffer->getName().c_str(), data.tar->pathname.c_str());
						} else {
							syslog(LOG_ERR, "BAD TAR POINTER %lx %s\n", (long)it->tar, data.buffer->getName().c_str());
						}
						tarthread->queue.erase(it++);
						unlock_okTarPointers();
						continue;
					}
					unlock_okTarPointers();
					isClosed = it->buffer->isClosed();
					lenForProceed = it->buffer->getChunkIterateLenForProceed();
					lenForProceedSafe = lenForProceed;
					if(!isClosed && lenForProceedSafe > tarChunk_kB * 1024) {
						 lenForProceedSafe = it->buffer->getChunkIterateSafeLimitLength(lenForProceedSafe);
					}
					if(isClosed ||
					   lenForProceedSafe > tarChunk_kB * 1024) {
						data = *it;
						findData = true;
						break;
					}
					it++;
				}
				if(isClosed && 
				   (!lenForProceed || lenForProceed > lenForProceedSafe)) {
					tarthread->queue.erase(it++);
				}
			}
			pthread_mutex_unlock(&tarthread->queuelock);
			if(!findData) {
				break;
			}
			
			Tar *tar = data.tar;
			tar->writing = 1;
			if(lenForProceedSafe) {
				//reset and set header
				memset(&(tar->tar.th_buf), 0, sizeof(struct Tar::tar_header));
				tar->th_set_type(0); //s->st_mode, 0 is regular file
				tar->th_set_user(0); //st_uid
				tar->th_set_group(0); //st_gid
				tar->th_set_mode(0); //s->st_mode
				tar->th_set_mtime(data.time);
				tar->th_set_size(lenForProceedSafe);
				tar->th_set_path((char*)data.filename.c_str(), !isClosed);
			       
				// write header
				if (tar->th_write() != 0) {
					goto end;
				}

				// if it's a regular file, write the contents as well
				tar->tar_append_buffer(data.buffer, lenForProceedSafe);
				
				if(sverb.chunk_buffer) {
					cout << " *** " << data.buffer->getName() << " " << lenForProceedSafe << endl;
				}
			}
end:
			tar->writing = 0;
			
			if(isClosed && 
			   (!lenForProceed || lenForProceed > lenForProceedSafe)) {
				decreaseTartimemap(data.buffer->getTime());
				if(sverb.tar > 2) {
					syslog(LOG_NOTICE, "tartimemap decrease1: %s %i %i %i %i", 
					       data.buffer->getName().c_str(), 
					       tar->created_at, tar->created_at - tar->created_at % TAR_MODULO_SECONDS,
					       data.buffer->getTime(), data.buffer->getTime() - data.buffer->getTime() % TAR_MODULO_SECONDS);
					if(tar->created_at != (unsigned)(data.buffer->getTime() - data.buffer->getTime() % TAR_MODULO_SECONDS)) {
						syslog(LOG_ERR, "BAD TAR created_at - tar: %s %lx %i %i chunkbuffer: %s %lx %i %i",
						       tar->pathname.c_str(), (long)tar, tar->created_at, tar->created_at - tar->created_at % TAR_MODULO_SECONDS,
						       data.buffer->getName().c_str(), (long)data.buffer, data.buffer->getTime(), data.buffer->getTime() - data.buffer->getTime() % TAR_MODULO_SECONDS);
					}
				}
				delete data.buffer;
				tar->incClosedPartCounter();
				__sync_sub_and_fetch(&glob_tar_queued_files, 1);
			}
		}
		// quque is empty - sleep before next run
		usleep(100000);
	}
	return NULL;
}

void
TarQueue::cleanTars() {
	// check if tar can be removed from map (check if there are still calls in memory) 
	if((last_flushTars + 10) > glob_last_packet_time) {
		// clean only each >10 seconds 
		return;
	}
//	if(sverb.tar) syslog(LOG_NOTICE, "cleanTars()");

	last_flushTars = glob_last_packet_time;
	map<string, Tar*>::iterator tars_it;
	pthread_mutex_lock(&tarslock);
	for(tars_it = tars.begin(); tars_it != tars.end();) {
		// walk through all tars
		Tar *tar = tars_it->second;
		pthread_mutex_lock(&tartimemaplock);
		unsigned int lpt = glob_last_packet_time;
		// find the tar in tartimemap 
		if((tartimemap.find(tar->created_at) == tartimemap.end()) and (lpt > (tar->created_at + TAR_MODULO_SECONDS + 10)) && // +10 seconds more in new period to be sure nothing is in buffers
		   tar->allPartsClosed()) {
			// there are no calls in this start time - clean it
			pthread_mutex_unlock(&tartimemaplock);
			if(tars_it->second->writing) {
				syslog(LOG_NOTICE, "fatal error! trying to close tar %s in the middle of writing data", tars_it->second->pathname.c_str());
			}
			if(sverb.tar) syslog(LOG_NOTICE, "destroying tar %s / %lx - (no calls in mem)\n", tars_it->second->pathname.c_str(), (long)tar);
			lock_okTarPointers();
			if(okTarPointers.find(tars_it->second) != okTarPointers.end()) {
				if(sverb.tar) syslog(LOG_NOTICE, "delete tar pointer %lx\n", (long)tars_it->second);
				okTarPointers.erase(tars_it->second);
			}
			if(sverb.tar <= 1) {
				delete tars_it->second;
			} else {
				tars_it->second->tar_close();
			}
			unlock_okTarPointers();
			tars.erase(tars_it++);
		} else {
			pthread_mutex_unlock(&tartimemaplock);
			tars_it++;
		}
	}
	pthread_mutex_unlock(&tarslock);
}

void   
TarQueue::flushQueue() {
	pthread_mutex_lock(&flushlock);
	// get candidate vector which has the biggest datalen in all files 
	int winner_qtype = 0;
	
	vector<data_t> winner;
	unsigned int winnertime = 0;
	size_t maxlen = 0;
	map<unsigned int, vector<data_t> >::iterator it;
	// walk all maps

	while(1) {
		lock();
		maxlen = 0;
		winnertime = 0;
		for(int i = 0; i < 4; i++) {
			//walk map
			for(it = queue[i].begin(); it != queue[i].end(); it++) {
				vector<data_t>::iterator itv;
				size_t sum = 0;
				for(itv = it->second.begin(); itv != it->second.end(); itv++) {
					sum += itv->buffer->getLen();
				}       
				if(sum > maxlen) {
					maxlen = sum;
					winnertime = it->first;
					winner = it->second;
					winner_qtype = i;
				}       
			}       
		}       

		if(maxlen > 0) {
			queue[winner_qtype][winnertime].clear();
			queue[winner_qtype].erase(winnertime);
			unlock();
			
			vector<data_t>::iterator itv;
			for(itv = winner.begin(); itv != winner.end(); itv++) {
				unsigned int time = itv->buffer->getTime();
				time -= time % TAR_MODULO_SECONDS;
				this->write(winner_qtype, time, *itv);
			}
			cleanTars();
			continue;
		} else {
			unlock();
			cleanTars();
			break;
		}
	}
	pthread_mutex_unlock(&flushlock);
}

int
TarQueue::queuelen() {
	int len = 0;
	for(int i = 0; i < 4; i++) {
		len += queue[i].size();
	}
	return len;
}

TarQueue::~TarQueue() {
	if(sverb.chunk_buffer > 1) { 
		cout << "destroy tar queue" << endl;
	}
	terminate = true;
	for(int i = 0; i < maxthreads; i++) { 
		pthread_join(tarthreads[i].thread, NULL);
		pthread_mutex_destroy(&tarthreads[i].queuelock);
	}

	pthread_mutex_destroy(&mutexlock);
	pthread_mutex_destroy(&flushlock);
	pthread_mutex_destroy(&tarslock);

	// destroy all tars
	for(map<string, Tar*>::iterator it = tars.begin(); it != tars.end(); it++) {
		delete(it->second);
	}

}      

TarQueue::TarQueue() {

	terminate = false;
	maxthreads = opt_pcap_dump_tar_threads;

	pthread_mutex_init(&mutexlock, NULL);
	pthread_mutex_init(&flushlock, NULL);
	pthread_mutex_init(&tarslock, NULL);
	last_flushTars = 0;
	for(int i = 0; i < maxthreads; i++) {
		tarthreadworker_arg *arg = new tarthreadworker_arg;
		arg->i = i;
		arg->tq = this;
		tarthreads[i].cpuPeak = 0;

		pthread_mutex_init(&tarthreads[i].queuelock, NULL);
		pthread_create(&tarthreads[i].thread, NULL, &TarQueue::tarthreadworker, arg);
		
		memset(this->tarthreads[i].threadPstatData, 0, sizeof(this->tarthreads[i].threadPstatData));
	}

	// create tarthreads
	
};	      

void TarQueue::preparePstatData(int threadIndex) {
	if(this->tarthreads[threadIndex].threadId) {
		if(this->tarthreads[threadIndex].threadPstatData[0].cpu_total_time) {
			this->tarthreads[threadIndex].threadPstatData[1] = this->tarthreads[threadIndex].threadPstatData[0];
		}
		pstat_get_data(this->tarthreads[threadIndex].threadId, this->tarthreads[threadIndex].threadPstatData);
	}
}

double TarQueue::getCpuUsagePerc(int threadIndex, bool preparePstatData) {
	if(preparePstatData) {
		this->preparePstatData(threadIndex);
	}
	if(this->tarthreads[threadIndex].threadId) {
		double ucpu_usage, scpu_usage;
		if(this->tarthreads[threadIndex].threadPstatData[0].cpu_total_time && this->tarthreads[threadIndex].threadPstatData[1].cpu_total_time) {
			pstat_calc_cpu_usage_pct(
				&this->tarthreads[threadIndex].threadPstatData[0], &this->tarthreads[threadIndex].threadPstatData[1],
				&ucpu_usage, &scpu_usage);
			double rslt = ucpu_usage + scpu_usage;
			if(rslt > this->tarthreads[threadIndex].cpuPeak) {
				this->tarthreads[threadIndex].cpuPeak = rslt;
			}
			return(rslt);
		}
	}
	return(-1);
}


void *TarQueueThread(void *dummy) {
	// run each second flushQueue
	while(!terminating) {
		tarQueue->flushQueue();
		sleep(1);
	}      
	return NULL;
}      

