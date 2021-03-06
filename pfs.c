/* Copyright (c) 2013-2014 Anton Titov.
 * Copyright (c) 2013-2014 pCloud Ltd.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of pCloud Ltd nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL pCloud Ltd BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64

#include <pthread.h>
#include <fuse.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include "pfs.h"
#include "pfsfolder.h"
#include "pcompat.h"
#include "plibs.h"
#include "psettings.h"
#include "pfsfolder.h"
#include "pcache.h"
#include "ppagecache.h"
#include "ptimer.h"
#include "pfstasks.h"
#include "pfsupload.h"

#if defined(P_OS_POSIX)
#include <signal.h>
#endif

#define fh_to_openfile(x) ((psync_openfile_t *)((uintptr_t)x))
#define openfile_to_fh(x) ((uintptr_t)x)

#define FS_BLOCK_SIZE 4096
#define FS_MAX_WRITE  256*1024

typedef struct {
  uint64_t offset;
  uint64_t length;
} index_record;

typedef struct {
  uint64_t copyfromoriginal;
} index_header;

static struct fuse_chan *psync_fuse_channel=0;
static struct fuse *psync_fuse=0;
static char *psync_current_mountpoint=0;

static pthread_mutex_t start_mutex=PTHREAD_MUTEX_INITIALIZER;
static int started=0;
static int initonce=0;

static uid_t myuid=0;
static gid_t mygid=0;

static psync_tree *openfiles=PSYNC_TREE_EMPTY;

int psync_fs_update_openfile(uint64_t taskid, uint64_t writeid, psync_fileid_t newfileid, uint64_t hash, uint64_t size){
  psync_openfile_t *fl;
  psync_tree *tr;
  psync_fsfileid_t fileid;
  int64_t d;
  int ret;
  fileid=-taskid;
  psync_sql_lock();
  tr=openfiles;
  while (tr){
    d=fileid-psync_tree_element(tr, psync_openfile_t, tree)->fileid;
    if (d<0)
      tr=tr->left;
    else if (d>0)
      tr=tr->right;
    else{
      fl=psync_tree_element(tr, psync_openfile_t, tree);
      pthread_mutex_lock(&fl->mutex);
      if (fl->writeid==writeid){
        fl->fileid=newfileid;
        fl->hash=hash;
        fl->modified=0;
        fl->newfile=0;
        fl->currentsize=size;
        fl->initialsize=size;
        fl->uploading=0;
        if (fl->datafile!=INVALID_HANDLE_VALUE){
          psync_file_close(fl->datafile);
          fl->datafile=INVALID_HANDLE_VALUE;
        }
        if (fl->indexfile!=INVALID_HANDLE_VALUE){
          psync_file_close(fl->indexfile);
          fl->indexfile=INVALID_HANDLE_VALUE;
        }
        ret=0;
      }
      else{
        fl->uploading=0;
        ret=-1;
      }
      pthread_mutex_unlock(&fl->mutex);
      psync_sql_unlock();
      return ret;
    }
  }
  psync_sql_unlock();
  return 0;
}

void psync_fs_uploading_openfile(uint64_t taskid){
  psync_openfile_t *fl;
  psync_tree *tr;
  psync_fsfileid_t fileid;
  int64_t d;
  fileid=-taskid;
  psync_sql_lock();
  tr=openfiles;
  while (tr){
    d=fileid-psync_tree_element(tr, psync_openfile_t, tree)->fileid;
    if (d<0)
      tr=tr->left;
    else if (d>0)
      tr=tr->right;
    else{
      fl=psync_tree_element(tr, psync_openfile_t, tree);
      pthread_mutex_lock(&fl->mutex);
      fl->uploading=1;
      pthread_mutex_unlock(&fl->mutex);
      break;
    }
  }
  psync_sql_unlock();
}

int psync_fs_rename_openfile_locked(psync_fsfileid_t fileid, psync_fsfolderid_t folderid, const char *name){
  psync_openfile_t *fl;
  psync_tree *tr;
  int64_t d;
  tr=openfiles;
  while (tr){
    d=fileid-psync_tree_element(tr, psync_openfile_t, tree)->fileid;
    if (d<0)
      tr=tr->left;
    else if (d>0)
      tr=tr->right;
    else{
      fl=psync_tree_element(tr, psync_openfile_t, tree);
      pthread_mutex_lock(&fl->mutex);
      if (fl->currentfolder->folderid!=folderid){
        psync_fstask_release_folder_tasks_locked(fl->currentfolder);
        fl->currentfolder=psync_fstask_get_or_create_folder_tasks_locked(folderid);
      }
      psync_free(fl->currentname);
      fl->currentname=psync_strdup(name);
      pthread_mutex_unlock(&fl->mutex);
      return 1;
    }
  }
  return 0;
}

int64_t psync_fs_get_file_writeid(uint64_t taskid){
  psync_openfile_t *fl;
  psync_tree *tr;
  psync_sql_res *res;
  psync_uint_row row;
  psync_fsfileid_t fileid;
  int64_t d;
  fileid=-taskid;
  psync_sql_lock();
  tr=openfiles;
  while (tr){
    d=fileid-psync_tree_element(tr, psync_openfile_t, tree)->fileid;
    if (d<0){
      if (tr->left)
        tr=tr->left;
      else
        break;
    }
    else if (d>0){
      if (tr->right)
        tr=tr->right;
      else
        break;
    }
    else{
      fl=psync_tree_element(tr, psync_openfile_t, tree);
      pthread_mutex_lock(&fl->mutex);
      d=fl->writeid;
      pthread_mutex_unlock(&fl->mutex);
      psync_sql_unlock();
      return d;
    }
  }
  res=psync_sql_query("SELECT int1 FROM fstask WHERE id=?");
  psync_sql_bind_uint(res, 1, taskid);
  if ((row=psync_sql_fetch_rowint(res)))
    d=row[0];
  else
    d=-1;
  psync_sql_free_result(res);
  psync_sql_unlock();
  return d;
}

static void psync_row_to_folder_stat(psync_variant_row row, struct stat *stbuf){
  memset(stbuf, 0, sizeof(struct stat));
#ifdef _DARWIN_FEATURE_64_BIT_INODE
  stbuf->st_birthtime=psync_get_number(row[2]);
  stbuf->st_ctime=psync_get_number(row[3]);
  stbuf->st_mtime=stbuf->st_ctime;
#else
  stbuf->st_ctime=psync_get_number(row[2]);
  stbuf->st_mtime=psync_get_number(row[3]);
#endif
  stbuf->st_mode=S_IFDIR | 0755;
  stbuf->st_nlink=psync_get_number(row[4])+2;
  stbuf->st_size=FS_BLOCK_SIZE;
#if defined(P_OS_POSIX)
  stbuf->st_blocks=1;
  stbuf->st_blksize=FS_BLOCK_SIZE;
#endif
  stbuf->st_uid=myuid;
  stbuf->st_gid=mygid;
}

static void psync_row_to_file_stat(psync_variant_row row, struct stat *stbuf){
  uint64_t size;
  size=psync_get_number(row[1]);
  memset(stbuf, 0, sizeof(struct stat));
#ifdef _DARWIN_FEATURE_64_BIT_INODE
  stbuf->st_birthtime=psync_get_number(row[2]);
  stbuf->st_ctime=psync_get_number(row[3]);
  stbuf->st_mtime=stbuf->st_ctime;
#else
  stbuf->st_ctime=psync_get_number(row[2]);
  stbuf->st_mtime=psync_get_number(row[3]);
#endif
  stbuf->st_mode=S_IFREG | 0644;
  stbuf->st_nlink=1;
  stbuf->st_size=size;
#if defined(P_OS_POSIX)
  stbuf->st_blocks=(size+511)/512;
  stbuf->st_blksize=FS_BLOCK_SIZE;
#endif
  stbuf->st_uid=myuid;
  stbuf->st_gid=mygid;
}

static void psync_mkdir_to_folder_stat(psync_fstask_mkdir_t *mk, struct stat *stbuf){
  memset(stbuf, 0, sizeof(struct stat));
#ifdef _DARWIN_FEATURE_64_BIT_INODE
  stbuf->st_birthtime=mk->ctime;
  stbuf->st_ctime=mk->mtime;
  stbuf->st_mtime=mk->mtime;
#else
  stbuf->st_ctime=mk->ctime;
  stbuf->st_mtime=mk->mtime;
#endif
  stbuf->st_mode=S_IFDIR | 0755;
  stbuf->st_nlink=mk->subdircnt+2;
  stbuf->st_size=FS_BLOCK_SIZE;
#if defined(P_OS_POSIX)
  stbuf->st_blocks=1;
  stbuf->st_blksize=FS_BLOCK_SIZE;
#endif
  stbuf->st_uid=myuid;
  stbuf->st_gid=mygid;
}

static int psync_creat_db_to_file_stat(psync_fileid_t fileid, struct stat *stbuf){
  psync_sql_res *res;
  psync_variant_row row;
  res=psync_sql_query("SELECT name, size, ctime, mtime FROM file WHERE id=?");
  psync_sql_bind_uint(res, 1, fileid);
  if ((row=psync_sql_fetch_row(res)))
    psync_row_to_file_stat(row, stbuf);
  psync_sql_free_result(res);
  return row?0:-1;
}

static int psync_creat_local_to_file_stat(psync_fstask_creat_t *cr, struct stat *stbuf){
  psync_stat_t st;
  psync_fsfileid_t fileid;
  uint64_t size, osize;
  const char *cachepath;
  char *filename;
  psync_file_t fd;
  char fileidhex[sizeof(psync_fsfileid_t)*2+2];
  int stret;
  fileid=cr->taskid;
  psync_binhex(fileidhex, &fileid, sizeof(psync_fsfileid_t));
  fileidhex[sizeof(psync_fsfileid_t)]='d';
  fileidhex[sizeof(psync_fsfileid_t)+1]=0;
  cachepath=psync_setting_get_string(_PS(fscachepath));
  filename=psync_strcat(cachepath, PSYNC_DIRECTORY_SEPARATOR, fileidhex, NULL);
  stret=psync_stat(filename, &st);
  psync_free(filename);
  if (stret)
    return -1;
  if (cr->newfile)
    osize=0;
  else{
    fileidhex[sizeof(psync_fsfileid_t)]='i';
    filename=psync_strcat(cachepath, PSYNC_DIRECTORY_SEPARATOR, fileidhex, NULL);
    fd=psync_file_open(filename, P_O_RDONLY, 0);
    psync_free(filename);
    if (fd==INVALID_HANDLE_VALUE)
      return -EIO;
    stret=psync_file_pread(fd, &osize, sizeof(osize), offsetof(index_header, copyfromoriginal));
    psync_file_close(fd);
    if (stret!=sizeof(osize))
      return -EIO;
  }
  memset(stbuf, 0, sizeof(struct stat));
#ifdef _DARWIN_FEATURE_64_BIT_INODE
  stbuf->st_birthtime=st.st_birthtime;
  stbuf->st_ctime=st.st_ctime;
  stbuf->st_mtime=st.st_mtime;
#else
  stbuf->st_ctime=psync_stat_ctime(&st);
  stbuf->st_mtime=psync_stat_mtime(&st);
#endif
  stbuf->st_mode=S_IFREG | 0644;
  stbuf->st_nlink=1;
  size=psync_stat_size(&st);
  if (osize>size)
    size=osize;
  stbuf->st_size=size;
#if defined(P_OS_POSIX)
  stbuf->st_blocks=(stbuf->st_size+511)/512;
  stbuf->st_blksize=FS_BLOCK_SIZE;
#endif
  stbuf->st_uid=myuid;
  stbuf->st_gid=mygid;
  return 0;
}

static int psync_creat_to_file_stat(psync_fstask_creat_t *cr, struct stat *stbuf){
  if (cr->fileid>=0)
    return psync_creat_db_to_file_stat(cr->fileid, stbuf);
  else
    return psync_creat_local_to_file_stat(cr, stbuf);
}

static int psync_fs_getrootattr(struct stat *stbuf){
  psync_sql_res *res;
  psync_variant_row row;
  res=psync_sql_query("SELECT 0, 0, 0, 0, subdircnt FROM folder WHERE id=0");
  if ((row=psync_sql_fetch_row(res)))
    psync_row_to_folder_stat(row, stbuf);
  psync_sql_free_result(res);
  return 0;
}

static int psync_fs_getattr(const char *path, struct stat *stbuf){
  psync_sql_res *res;
  psync_variant_row row;
  psync_fspath_t *fpath;
  psync_fstask_folder_t *folder;
  psync_fstask_creat_t *cr;
  int crr;
//  debug(D_NOTICE, "getattr %s", path);
  if (path[0]=='/' && path[1]==0)
    return psync_fs_getrootattr(stbuf);
  psync_sql_lock();
  fpath=psync_fsfolder_resolve_path(path);
  if (!fpath){
    psync_sql_unlock();
    return -ENOENT;
  }
  folder=psync_fstask_get_folder_tasks_locked(fpath->folderid);
  if (!folder || !psync_fstask_find_rmdir(folder, fpath->name, 0)){
    res=psync_sql_query("SELECT id, permissions, ctime, mtime, subdircnt FROM folder WHERE parentfolderid=? AND name=?");
    psync_sql_bind_uint(res, 1, fpath->folderid);
    psync_sql_bind_string(res, 2, fpath->name);
    if ((row=psync_sql_fetch_row(res)))
      psync_row_to_folder_stat(row, stbuf);
    psync_sql_free_result(res);
    if (row){
      if (folder)
        psync_fstask_release_folder_tasks_locked(folder);
      psync_sql_unlock();
      psync_free(fpath);
      return 0;
    }
  }
  if (folder){
    psync_fstask_mkdir_t *mk;
    mk=psync_fstask_find_mkdir(folder, fpath->name, 0);
    if (mk){
      psync_mkdir_to_folder_stat(mk, stbuf);
      psync_fstask_release_folder_tasks_locked(folder);
      psync_sql_unlock();
      psync_free(fpath);
      return 0;
    }
  }
  res=psync_sql_query("SELECT name, size, ctime, mtime FROM file WHERE parentfolderid=? AND name=?");
  psync_sql_bind_uint(res, 1, fpath->folderid);
  psync_sql_bind_string(res, 2, fpath->name);
  if ((row=psync_sql_fetch_row(res)))
    psync_row_to_file_stat(row, stbuf);
  psync_sql_free_result(res);
  if (folder){
    if (psync_fstask_find_unlink(folder, fpath->name, 0))
      row=NULL;
    if (!row && (cr=psync_fstask_find_creat(folder, fpath->name, 0)))
      crr=psync_creat_to_file_stat(cr, stbuf);
    else
      crr=-1;
    psync_fstask_release_folder_tasks_locked(folder);
  }
  else
    crr=-1;
  psync_sql_unlock();
  psync_free(fpath);
  if (row || !crr)
    return 0;
  debug(D_NOTICE, "returning ENOENT for %s", path);
  return -ENOENT;
}

static int psync_fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi){
  psync_sql_res *res;
  psync_variant_row row;
  psync_fsfolderid_t folderid;
  psync_fstask_folder_t *folder;
  psync_tree *trel;
  const char *name;
  struct stat st;
  debug(D_NOTICE, "readdir %s", path);
  psync_sql_lock();
  folderid=psync_fsfolderid_by_path(path);
  if (unlikely_log(folderid==PSYNC_INVALID_FSFOLDERID)){
    psync_sql_unlock();
    return -ENOENT;
  }
  filler(buf, ".", NULL, 0);
  filler(buf, "..", NULL, 0);
  folder=psync_fstask_get_folder_tasks_locked(folderid);
  if (folderid>=0){
    res=psync_sql_query("SELECT name, permissions, ctime, mtime, subdircnt FROM folder WHERE parentfolderid=?");
    psync_sql_bind_uint(res, 1, folderid);
    while ((row=psync_sql_fetch_row(res))){
      name=psync_get_string(row[0]);
      if (folder && (psync_fstask_find_rmdir(folder, name, 0) || psync_fstask_find_mkdir(folder, name, 0)))
        continue;
      psync_row_to_folder_stat(row, &st);
      filler(buf, name, &st, 0);
    }
    psync_sql_free_result(res);
    res=psync_sql_query("SELECT name, size, ctime, mtime FROM file WHERE parentfolderid=?");
    psync_sql_bind_uint(res, 1, folderid);
    while ((row=psync_sql_fetch_row(res))){
      name=psync_get_string(row[0]);
      if (folder && psync_fstask_find_unlink(folder, name, 0))
        continue;
      psync_row_to_file_stat(row, &st);
      filler(buf, name, &st, 0);
    }
    psync_sql_free_result(res);
  }
  if (folder){
    psync_tree_for_each(trel, folder->mkdirs){
      psync_mkdir_to_folder_stat(psync_tree_element(trel, psync_fstask_mkdir_t, tree), &st);
      filler(buf, psync_tree_element(trel, psync_fstask_mkdir_t, tree)->name, &st, 0);
    }
    psync_tree_for_each(trel, folder->creats){
      if (!psync_creat_to_file_stat(psync_tree_element(trel, psync_fstask_creat_t, tree), &st))
        filler(buf, psync_tree_element(trel, psync_fstask_creat_t, tree)->name, &st, 0);
    }
    psync_fstask_release_folder_tasks_locked(folder);
  }
  psync_sql_unlock();
  return 0;
}

static psync_openfile_t *psync_fs_create_file(psync_fsfileid_t fileid, psync_fsfileid_t remotefileid, uint64_t size, uint64_t hash, int lock, psync_fstask_folder_t *folder, const char *name){
  psync_openfile_t *fl;
  psync_tree *tr;
  int64_t d;
  psync_sql_lock();
  tr=openfiles;
  d=-1;
  while (tr){
    d=fileid-psync_tree_element(tr, psync_openfile_t, tree)->fileid;
    if (d<0){
      if (tr->left)
        tr=tr->left;
      else
        break;
    }
    else if (d>0){
      if (tr->right)
        tr=tr->right;
      else
        break;
    }
    else{
      fl=psync_tree_element(tr, psync_openfile_t, tree);
      if (lock){
        pthread_mutex_lock(&fl->mutex);
        psync_fs_inc_of_refcnt_locked(fl);
      }
      else
        psync_fs_inc_of_refcnt(fl);
      assertw(fl->currentfolder==folder);
      assertw(!strcmp(fl->currentname, name));
      psync_fstask_release_folder_tasks_locked(folder);
      psync_sql_unlock();
      debug(D_NOTICE, "found open file %ld, refcnt %u", (long int)fileid, (unsigned)fl->refcnt);
      return fl;
    }
  }
  fl=psync_new(psync_openfile_t);
  memset(fl, 0, sizeof(psync_openfile_t));
  if (d<0)
    psync_tree_add_before(&openfiles, tr, &fl->tree);
  else
    psync_tree_add_after(&openfiles, tr, &fl->tree);
  pthread_mutex_init(&fl->mutex, NULL);
  fl->currentfolder=folder;
  fl->currentname=psync_strdup(name);
  fl->fileid=fileid;
  fl->remotefileid=remotefileid;
  fl->hash=hash;
  fl->initialsize=size;
  fl->currentsize=size;
  fl->datafile=INVALID_HANDLE_VALUE;
  fl->indexfile=INVALID_HANDLE_VALUE;
  fl->refcnt=1;
  fl->modified=fileid<0?1:0;
  if (lock)
    pthread_mutex_lock(&fl->mutex);
  psync_sql_unlock();
  return fl;
}

int64_t psync_fs_load_interval_tree(psync_file_t fd, uint64_t size, psync_interval_tree_t **tree){
  index_record records[512];
  uint64_t cnt;
  uint64_t i;
  ssize_t rrd, rd, j;
  if (size<sizeof(index_header))
    return 0;
  size-=sizeof(index_header);
  assertw(size%sizeof(index_record)==0);
  cnt=size/sizeof(index_record);
  debug(D_NOTICE, "loading %lu intervals", (unsigned long)cnt);
  for (i=0; i<cnt; i+=ARRAY_SIZE(records)){
    rd=ARRAY_SIZE(records)>cnt-i?cnt-i:ARRAY_SIZE(records);
    rrd=psync_file_pread(fd, records, rd*sizeof(index_record), i*sizeof(index_record)+sizeof(index_header));
    if (unlikely_log(rrd!=rd*sizeof(index_record)))
      return -1;
    for (j=0; j<rd; j++)
      psync_interval_tree_add(tree, records[j].offset, records[j].offset+records[j].length);
  }
  return cnt;
}

static int load_interval_tree(psync_openfile_t *of){
  index_header hdr;
  int64_t ifs;
  ifs=psync_file_size(of->indexfile);
  if (unlikely_log(ifs==-1))
    return -1;
  if (ifs<sizeof(index_header)){
    assertw(ifs==0);
    hdr.copyfromoriginal=of->initialsize;
    if (psync_file_pwrite(of->indexfile, &hdr, sizeof(index_header), 0)!=sizeof(index_header))
      return -1;
    else
      return 0;
  }
  ifs=psync_fs_load_interval_tree(of->indexfile, ifs, &of->writeintervals);
  if (ifs==-1)
    return -1;
  else{
    of->indexoff=ifs;
    return 0;
  }
}

static int open_write_files(psync_openfile_t *of, int trunc){
  psync_fsfileid_t fileid;
  const char *cachepath;
  char *filename;
  char fileidhex[sizeof(psync_fsfileid_t)*2+2];
  fileid=-of->fileid;
  psync_binhex(fileidhex, &fileid, sizeof(psync_fsfileid_t));
  fileidhex[sizeof(psync_fsfileid_t)]='d';
  fileidhex[sizeof(psync_fsfileid_t)+1]=0;
  cachepath=psync_setting_get_string(_PS(fscachepath));
  if (of->datafile==INVALID_HANDLE_VALUE){
    filename=psync_strcat(cachepath, PSYNC_DIRECTORY_SEPARATOR, fileidhex, NULL);
    of->datafile=psync_file_open(filename, P_O_RDWR, P_O_CREAT|(trunc?P_O_TRUNC:0));
    psync_free(filename);
    if (of->datafile==INVALID_HANDLE_VALUE){
      debug(D_ERROR, "could not open cache file %s", filename);
      return -EIO;
    }
    of->currentsize=psync_file_size(of->datafile);
  }
  if (!of->newfile && of->indexfile==INVALID_HANDLE_VALUE){
    fileidhex[sizeof(psync_fsfileid_t)]='i';
    filename=psync_strcat(cachepath, PSYNC_DIRECTORY_SEPARATOR, fileidhex, NULL);
    of->indexfile=psync_file_open(filename, P_O_RDWR, P_O_CREAT|(trunc?P_O_TRUNC:0));
    psync_free(filename);
    if (of->indexfile==INVALID_HANDLE_VALUE){
      debug(D_ERROR, "could not open cache file %s", filename);
      return -EIO;
    }
    if (load_interval_tree(of)){
      debug(D_ERROR, "could not load cache file %s to interval tree", filename);
      return -EIO;
    }
  }
  return 0;
}

static int psync_fs_open(const char *path, struct fuse_file_info *fi){
  psync_sql_res *res;
  psync_uint_row row;
  psync_fileid_t fileid;
  uint64_t size, hash, writeid;
  psync_fspath_t *fpath;
  psync_fstask_creat_t *cr;
  psync_fstask_folder_t *folder;
  psync_openfile_t *of;
  int ret;
  debug(D_NOTICE, "open %s", path);
  psync_sql_lock();
  fpath=psync_fsfolder_resolve_path(path);
  if (!fpath){
    debug(D_NOTICE, "returning ENOENT for %s, folder not found", path);
    psync_sql_unlock();
    return -ENOENT;
  }
  if ((fi->flags&3)!=O_RDONLY && !(fpath->permissions&PSYNC_PERM_MODIFY)){
    psync_sql_unlock();
    psync_free(fpath);
    return -EACCES;
  }
  folder=psync_fstask_get_or_create_folder_tasks_locked(fpath->folderid);
  if (fpath->folderid>=0 && !psync_fstask_find_unlink(folder, fpath->name, 0)){
    res=psync_sql_query("SELECT id, size, hash FROM file WHERE parentfolderid=? AND name=?");
    psync_sql_bind_uint(res, 1, fpath->folderid);
    psync_sql_bind_string(res, 2, fpath->name);
    row=psync_sql_fetch_rowint(res);
    if (row){
      fileid=row[0];
      size=row[1];
      hash=row[2];
      debug(D_NOTICE, "opening regular file %lu %s", (unsigned long)fileid, fpath->name);
    }
    psync_sql_free_result(res);
  }
  else
    row=NULL;
  if (!row && (cr=psync_fstask_find_creat(folder, fpath->name, 0))){
    if (cr->fileid>=0){
      res=psync_sql_query("SELECT id, size, hash FROM file WHERE id=?");
      psync_sql_bind_uint(res, 1, cr->fileid);
      row=psync_sql_fetch_rowint(res);
      if (row){
        fileid=row[0];
        size=row[1];
        hash=row[2];
        debug(D_NOTICE, "opening moved regular file %lu %s", (unsigned long)fileid, fpath->name);
      }
      psync_sql_free_result(res);
    }
    else if (cr->newfile){
      fileid=cr->fileid;
      of=psync_fs_create_file(fileid, 0, 0, 0, 1, psync_fstask_get_ref_locked(folder), fpath->name);
      psync_fstask_release_folder_tasks_locked(folder);
      psync_sql_unlock();
      debug(D_NOTICE, "opening new file %ld %s", (long)fileid, fpath->name);
      psync_free(fpath);
      of->newfile=1;
      ret=open_write_files(of, fi->flags&O_TRUNC);
      pthread_mutex_unlock(&of->mutex);
      fi->fh=openfile_to_fh(of);
      if (unlikely_log(ret)){
        psync_fs_dec_of_refcnt(of);
        return ret;
      }
      else
        return ret;
    }
    else{
      debug(D_NOTICE, "opening sparse file %ld %s", (long)cr->fileid, fpath->name);
      fileid=writeid=hash=size=0; // prevent (stupid) warnings
      res=psync_sql_query("SELECT fileid, int1, int2 FROM fstask WHERE id=?");
      psync_sql_bind_uint(res, 1, -cr->fileid);
      row=psync_sql_fetch_rowint(res);
      if (row){
        fileid=row[0];
        writeid=row[1];
        hash=row[2];
      }
      psync_sql_free_result(res);
      if (unlikely_log(!row)){
        ret=-ENOENT;
        goto ex0;
      }
      if (fi->flags&O_TRUNC)
        size=0;
      else{
        res=psync_sql_query("SELECT size FROM filerevision WHERE fileid=? AND hash=?");
        psync_sql_bind_uint(res, 1, fileid);
        psync_sql_bind_uint(res, 2, hash);
        row=psync_sql_fetch_rowint(res);
        if (row)
          size=row[0];
        psync_sql_free_result(res);
        if (unlikely_log(!row)){
          ret=-ENOENT;
          goto ex0;
        }
      }
      of=psync_fs_create_file(cr->fileid, fileid, size, hash, 1, psync_fstask_get_ref_locked(folder), fpath->name);
      psync_fstask_release_folder_tasks_locked(folder);
      psync_sql_unlock();
      psync_free(fpath);
      of->newfile=0;
      of->writeid=writeid;
      ret=open_write_files(of, fi->flags&O_TRUNC);
      pthread_mutex_unlock(&of->mutex);
      fi->fh=openfile_to_fh(of);
      if (unlikely_log(ret)){
        psync_fs_dec_of_refcnt(of);
        return ret;
      }
      else
        return ret;

    }
  }
  if (row){
    of=psync_fs_create_file(fileid, fileid, size, hash, 0, psync_fstask_get_ref_locked(folder), fpath->name);
    fi->fh=openfile_to_fh(of);
    ret=0;
  }
  else
    ret=-ENOENT;
ex0:
  psync_fstask_release_folder_tasks_locked(folder);
  psync_sql_unlock();
  psync_free(fpath);
  return ret;
}

static int psync_fs_creat(const char *path, mode_t mode, struct fuse_file_info *fi){
  psync_fspath_t *fpath;
  psync_fstask_folder_t *folder;
  psync_fstask_creat_t *cr;
  psync_openfile_t *of;
  int ret;
  debug(D_NOTICE, "creat %s", path);
  psync_sql_lock();
  fpath=psync_fsfolder_resolve_path(path);
  if (!fpath){
    debug(D_NOTICE, "returning ENOENT for %s, folder not found", path);
    psync_sql_unlock();
    return -ENOENT;
  }
  if (!(fpath->permissions&PSYNC_PERM_CREATE)){
    psync_sql_unlock();
    psync_free(fpath);
    return -EACCES;
  }
  folder=psync_fstask_get_or_create_folder_tasks_locked(fpath->folderid);
  //TODO: check if file exists
  cr=psync_fstask_add_creat(folder, fpath->name);
  if (unlikely_log(!cr)){
    psync_fstask_release_folder_tasks_locked(folder);
    psync_sql_unlock();
    return -EIO;
  }
  of=psync_fs_create_file(cr->fileid, 0, 0, 0, 1, psync_fstask_get_ref_locked(folder), fpath->name);
  psync_fstask_release_folder_tasks_locked(folder);
  psync_sql_unlock();
  of->newfile=1;
  of->modified=1;
  ret=open_write_files(of, 1);
  pthread_mutex_unlock(&of->mutex);
  if (unlikely_log(ret)){
    psync_sql_lock();
    folder=psync_fstask_get_or_create_folder_tasks_locked(fpath->folderid);
    if (folder){
      if ((cr=psync_fstask_find_creat(folder, fpath->name, 0))){
        psync_tree_del(&folder->creats, &cr->tree);
        psync_free(cr);
      }
      psync_fstask_release_folder_tasks_locked(folder);
    }
    psync_fs_dec_of_refcnt(of);
    psync_sql_unlock();
    psync_free(fpath);
    return ret;
  }
  psync_free(fpath);
  fi->fh=openfile_to_fh(of);
  return 0;
}

void psync_fs_inc_of_refcnt_locked(psync_openfile_t *of){
  of->refcnt++;
}

void psync_fs_inc_of_refcnt(psync_openfile_t *of){
  pthread_mutex_lock(&of->mutex);
  psync_fs_inc_of_refcnt_locked(of);
  pthread_mutex_unlock(&of->mutex);
}

static void psync_fs_free_openfile(psync_openfile_t *of){
  debug(D_NOTICE, "releasing file %s", of->currentname);
  pthread_mutex_destroy(&of->mutex);
  if (of->datafile!=INVALID_HANDLE_VALUE)
    psync_file_close(of->datafile);
  if (of->indexfile!=INVALID_HANDLE_VALUE)
    psync_file_close(of->indexfile);
  if (of->writeintervals)
    psync_interval_tree_free(of->writeintervals);
  psync_fstask_release_folder_tasks(of->currentfolder);
  psync_free(of->currentname);
  psync_free(of);
}

void psync_fs_dec_of_refcnt(psync_openfile_t *of){
  uint32_t refcnt;
  psync_sql_lock();
  pthread_mutex_lock(&of->mutex);
  refcnt=--of->refcnt;
  if (refcnt==0)
    psync_tree_del(&openfiles, &of->tree);
  psync_sql_unlock();
  pthread_mutex_unlock(&of->mutex);
  if (!refcnt)
    psync_fs_free_openfile(of);
}

void psync_fs_inc_of_refcnt_and_readers(psync_openfile_t *of){
  pthread_mutex_lock(&of->mutex);
  of->refcnt++;
  of->runningreads++;
  pthread_mutex_unlock(&of->mutex);
}

void psync_fs_dec_of_refcnt_and_readers(psync_openfile_t *of){
  uint32_t refcnt;
  psync_sql_lock();
  pthread_mutex_lock(&of->mutex);
  of->runningreads--;
  refcnt=--of->refcnt;
  if (refcnt==0)
    psync_tree_del(&openfiles, &of->tree);
  psync_sql_unlock();
  pthread_mutex_unlock(&of->mutex);
  if (!refcnt)
    psync_fs_free_openfile(of);
}

static int psync_fs_release(const char *path, struct fuse_file_info *fi){
  debug(D_NOTICE, "release %s", path);
  psync_fs_dec_of_refcnt(fh_to_openfile(fi->fh));
  return 0;
}

static int psync_fs_flush(const char *path, struct fuse_file_info *fi){
  psync_openfile_t *of;
  debug(D_NOTICE, "flush %s", path);
  of=fh_to_openfile(fi->fh);
  pthread_mutex_lock(&of->mutex);
  if (of->modified && !of->uploading){
    psync_sql_res *res;
    uint64_t writeid;
    uint32_t aff;
    writeid=of->writeid;
    pthread_mutex_unlock(&of->mutex);
    debug(D_NOTICE, "releasing new file %s for upload", path);
    res=psync_sql_prep_statement("UPDATE fstask SET status=0, int1=? WHERE id=? AND status=1");
    psync_sql_bind_uint(res, 1, writeid);
    psync_sql_bind_uint(res, 2, -of->fileid);
    psync_sql_run(res);
    aff=psync_sql_affected_rows();
    psync_sql_free_result(res);
    if (aff)
      psync_fsupload_wake();
    else{
      res=psync_sql_prep_statement("UPDATE fstask SET int1=? WHERE id=? AND int1<?");
      psync_sql_bind_uint(res, 1, writeid);
      psync_sql_bind_uint(res, 2, -of->fileid);
      psync_sql_bind_uint(res, 3, writeid);
      psync_sql_run_free(res);
    }
    return 0;
  }
  pthread_mutex_unlock(&of->mutex);
  return 0;
}

static int psync_fs_fsync(const char *path, int datasync, struct fuse_file_info *fi){
  psync_openfile_t *of;
  debug(D_NOTICE, "fsync %s", path);
  of=fh_to_openfile(fi->fh);
  pthread_mutex_lock(&of->mutex);
  if (!of->modified){
    pthread_mutex_unlock(&of->mutex);
    return 0;
  }
  if (unlikely_log(psync_file_sync(of->datafile)) || unlikely_log(!of->newfile && psync_file_sync(of->indexfile))){
    pthread_mutex_unlock(&of->mutex);
    return -EIO;
  }
  pthread_mutex_unlock(&of->mutex);
  if (unlikely_log(psync_sql_sync()))
    return -EIO;
  return 0;
}

static int psync_fs_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi){
  debug(D_NOTICE, "fsyncdir %s", path);
  if (unlikely_log(psync_sql_sync()))
    return -EIO;
  else
    return 0;
}

static int psync_read_newfile(psync_openfile_t *of, char *buf, uint64_t size, uint64_t offset){
  ssize_t br=psync_file_pread(of->datafile, buf, size, offset);
  if (br==-1)
    return -EIO;
  else
    return br;
}

static int psync_fs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
  psync_openfile_t *of;
  time_t currenttime;
  of=fh_to_openfile(fi->fh);
  currenttime=psync_timer_time();
  pthread_mutex_lock(&of->mutex);
  if (of->currentsec==currenttime){
    of->bytesthissec+=size;
    if (of->currentspeed<of->bytesthissec)
      of->currentspeed=of->bytesthissec;
  }
  else{
    if (of->currentsec<currenttime-10)
      of->currentspeed=size;
    else if (of->currentspeed==0)
      of->currentspeed=of->bytesthissec;
    else
      of->currentspeed=(of->bytesthissec/(currenttime-of->currentsec)+of->currentspeed*3)/4;
    of->currentsec=currenttime;
    of->bytesthissec=size;
  }
  if (of->newfile){
    int ret=psync_read_newfile(of, buf, size, offset);
    pthread_mutex_unlock(&of->mutex);
    return ret;
  }
  if (of->modified)
    return psync_pagecache_read_modified_locked(of, buf, size, offset);
  else
    return psync_pagecache_read_unmodified_locked(of, buf, size, offset);
}

static int psync_fs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
  psync_openfile_t *of;
  ssize_t bw;
  uint64_t ioff;
  index_record rec;
  int ret;
  of=fh_to_openfile(fi->fh);
  pthread_mutex_lock(&of->mutex);
  if (unlikely(of->uploading)){
    pthread_mutex_unlock(&of->mutex);
    //debug(D_NOTICE, "stopping upload of file %s as new write arrived", path);
    //TODO: stop upload
    pthread_mutex_lock(&of->mutex);
  }
  of->writeid++;
retry:
  if (of->newfile){
    bw=psync_file_pwrite(of->datafile, buf, size, offset);
    pthread_mutex_unlock(&of->mutex);
    if (unlikely_log(bw==-1))
      return -EIO;
    else
      return bw;
  }
  else{
    if (unlikely(!of->modified)){
      psync_fstask_creat_t *cr;
      debug(D_NOTICE, "reopening file %s for writing", of->currentname);
      if (psync_sql_trylock()){
        // we have to take sql_lock and retake of->mutex AFTER, then check if the case is still !of->newfile && !of->modified
        pthread_mutex_unlock(&of->mutex);
        psync_sql_lock();
        pthread_mutex_lock(&of->mutex);
        if (of->newfile || of->modified){
          psync_sql_unlock();
          goto retry;
        }
      }
      cr=psync_fstask_add_modified_file(of->currentfolder, of->currentname, of->fileid, of->hash);
      psync_sql_unlock();
      if (unlikely_log(!cr)){
        pthread_mutex_unlock(&of->mutex);
        return -EIO;
      }
      of->fileid=cr->fileid;
      ret=open_write_files(of, 0);
      if (unlikely_log(ret) || psync_file_seek(of->datafile, of->initialsize, P_SEEK_SET)==-1 || psync_file_truncate(of->datafile)){
        pthread_mutex_unlock(&of->mutex);
        return ret;
      }
      of->modified=1;
      of->indexoff=0;
    }
    ioff=of->indexoff++;
    bw=psync_file_pwrite(of->datafile, buf, size, offset);
    if (unlikely_log(bw==-1)){
      pthread_mutex_unlock(&of->mutex);
      return -EIO;
    }
    rec.offset=offset;
    rec.length=bw;
    if (unlikely_log(psync_file_pwrite(of->indexfile, &rec, sizeof(rec), sizeof(rec)*ioff+sizeof(index_header))!=sizeof(rec))){
      pthread_mutex_unlock(&of->mutex);
      return -EIO;
    }
    psync_interval_tree_add(&of->writeintervals, offset, offset+bw);
    pthread_mutex_unlock(&of->mutex);
    return bw;
  }
}

static int psync_fs_mkdir(const char *path, mode_t mode){
  psync_fspath_t *fpath;
  int ret;
  debug(D_NOTICE, "mkdir %s", path);
  psync_sql_lock();
  fpath=psync_fsfolder_resolve_path(path);
  if (!fpath){
    psync_sql_unlock();
    debug(D_NOTICE, "returning ENOENT for %s, folder not found", path);
    return -ENOENT;
  }
  if (!(fpath->permissions&PSYNC_PERM_CREATE)){
    psync_sql_unlock();
    psync_free(fpath);
    return -EACCES;
  }
  ret=psync_fstask_mkdir(fpath->folderid, fpath->name);
  psync_sql_unlock();
  psync_free(fpath);
  return ret;
}

static int psync_fs_rmdir(const char *path){
  psync_fspath_t *fpath;
  int ret;
  debug(D_NOTICE, "rmdir %s", path);
  psync_sql_lock();
  fpath=psync_fsfolder_resolve_path(path);
  if (!fpath){
    psync_sql_unlock();
    debug(D_NOTICE, "returning ENOENT for %s, folder not found", path);
    return -ENOENT;
  }
  if (!(fpath->permissions&PSYNC_PERM_DELETE)){
    psync_sql_unlock();
    psync_free(fpath);
    return -EACCES;
  }
  ret=psync_fstask_rmdir(fpath->folderid, fpath->name);
  psync_sql_unlock();
  psync_free(fpath);
  return ret;
}

static int psync_fs_unlink(const char *path){
  psync_fspath_t *fpath;
  int ret;
  debug(D_NOTICE, "unlink %s", path);
  psync_sql_lock();
  fpath=psync_fsfolder_resolve_path(path);
  if (!fpath){
    psync_sql_unlock();
    debug(D_NOTICE, "returning ENOENT for %s, folder not found", path);
    return -ENOENT;
  }
  if (!(fpath->permissions&PSYNC_PERM_DELETE)){
    psync_sql_unlock();
    psync_free(fpath);
    return -EACCES;
  }
  ret=psync_fstask_unlink(fpath->folderid, fpath->name);
  psync_sql_unlock();
  psync_free(fpath);
  return ret;
}

static int psync_fs_rename_folder(psync_fsfolderid_t folderid, psync_fsfolderid_t parentfolderid, const char *name, uint32_t src_permissions,
                                  psync_fsfolderid_t to_folderid, const char *new_name, uint32_t targetperms){
  if (parentfolderid==to_folderid){
    assertw(targetperms==src_permissions);
    if (!(src_permissions&PSYNC_PERM_MODIFY))
      return -EACCES;
  }
  else{
    if (!(src_permissions&PSYNC_PERM_DELETE) || !(targetperms&PSYNC_PERM_CREATE))
      return -EACCES;
  }
  return psync_fstask_rename_folder(folderid, parentfolderid, name, to_folderid, new_name);
}

static int psync_fs_rename_file(psync_fsfileid_t fileid, psync_fsfolderid_t parentfolderid, const char *name, uint32_t src_permissions,
                                  psync_fsfolderid_t to_folderid, const char *new_name, uint32_t targetperms){
  if (parentfolderid==to_folderid){
    assertw(targetperms==src_permissions);
    if (!(src_permissions&PSYNC_PERM_MODIFY))
      return -EACCES;
  }
  else{
    if (!(src_permissions&PSYNC_PERM_DELETE) || !(targetperms&PSYNC_PERM_CREATE))
      return -EACCES;
  }
  return psync_fstask_rename_file(fileid, parentfolderid, name, to_folderid, new_name);
}

static int psync_fs_rename(const char *old_path, const char *new_path){
  psync_fspath_t *fold_path, *fnew_path;
  psync_sql_res *res;
  psync_fstask_folder_t *folder;
  psync_fstask_mkdir_t *mkdir;
  psync_fstask_creat_t *creat;
  psync_fsfolderid_t to_folderid;
  psync_uint_row row;
  const char *new_name;
  psync_fileorfolderid_t fid;
  uint32_t targetperms;
  int ret;
  debug(D_NOTICE, "rename %s to %s", old_path, new_path);
  folder=NULL;
  psync_sql_lock();
  fold_path=psync_fsfolder_resolve_path(old_path);
  fnew_path=psync_fsfolder_resolve_path(new_path);
  if (!fold_path || !fnew_path){
    if (fold_path && new_path[1]==0 && new_path[0]=='/'){
      to_folderid=0;
      targetperms=PSYNC_PERM_ALL;
      new_name=NULL;
      goto move_to_root;
    }
    goto err_enoent;
  }
  folder=psync_fstask_get_folder_tasks_locked(fnew_path->folderid);
  if (folder && (mkdir=psync_fstask_find_mkdir(folder, fnew_path->name, 0))){
    to_folderid=mkdir->folderid;
    if (mkdir->folderid>0){
      res=psync_sql_query("SELECT permissions FROM folder WHERE id=?");
      psync_sql_bind_uint(res, 1, mkdir->folderid);
      if ((row=psync_sql_fetch_rowint(res)))
        targetperms=row[0]&fnew_path->permissions;
      psync_sql_free_result(res);
      if (!row)
        goto err_enoent;
    }
    else
      targetperms=fnew_path->permissions;
    new_name=NULL;
  }
  else if (fnew_path->folderid>=0){
    res=psync_sql_query("SELECT id, permissions FROM folder WHERE parentfolderid=? AND name=?");
    psync_sql_bind_uint(res, 1, fnew_path->folderid);
    psync_sql_bind_string(res, 2, fnew_path->name);
    row=psync_sql_fetch_rowint(res);
    if (row && (!folder || !psync_fstask_find_rmdir(folder, fnew_path->name, 0))){
      to_folderid=row[0];
      targetperms=row[1]&fnew_path->permissions;
      new_name=NULL;
    }
    else{
      to_folderid=fnew_path->folderid;
      targetperms=fnew_path->permissions;
      new_name=fnew_path->name;
    }
    psync_sql_free_result(res);
  }
  else{
    to_folderid=fnew_path->folderid;
    targetperms=fnew_path->permissions;
    new_name=fnew_path->name;
  }
  if (folder)
    psync_fstask_release_folder_tasks_locked(folder);
move_to_root:
  folder=psync_fstask_get_folder_tasks_locked(fold_path->folderid);
  if (folder){
    if ((mkdir=psync_fstask_find_mkdir(folder, fold_path->name, 0))){
      ret=psync_fs_rename_folder(mkdir->folderid, fold_path->folderid, fold_path->name, fold_path->permissions, to_folderid, new_name, targetperms);
      goto finish;
    }
    else if ((creat=psync_fstask_find_creat(folder, fold_path->name, 0))){
      ret=psync_fs_rename_file(creat->fileid, fold_path->folderid, fold_path->name, fold_path->permissions, to_folderid, new_name, targetperms);
      goto finish;
    }
  }
  if (!folder || !psync_fstask_find_rmdir(folder, fold_path->name, 0)){
    // even if we don't use permissions, it is probably better to use same query as above
    res=psync_sql_query("SELECT id, permissions FROM folder WHERE parentfolderid=? AND name=?");
    psync_sql_bind_uint(res, 1, fold_path->folderid);
    psync_sql_bind_string(res, 2, fold_path->name);
    if ((row=psync_sql_fetch_rowint(res))){
      fid=row[0];
      psync_sql_free_result(res);
      ret=psync_fs_rename_folder(fid, fold_path->folderid, fold_path->name, fold_path->permissions, to_folderid, new_name, targetperms);
      goto finish;
    }
    psync_sql_free_result(res);
  }
  if (!folder || !psync_fstask_find_unlink(folder, fold_path->name, 0)){
    res=psync_sql_query("SELECT id FROM file WHERE parentfolderid=? AND name=?");
    psync_sql_bind_uint(res, 1, fold_path->folderid);
    psync_sql_bind_string(res, 2, fold_path->name);
    if ((row=psync_sql_fetch_rowint(res))){
      fid=row[0];
      psync_sql_free_result(res);
      ret=psync_fs_rename_file(fid, fold_path->folderid, fold_path->name, fold_path->permissions, to_folderid, new_name, targetperms);
      goto finish;
    }
    psync_sql_free_result(res);
  }
  goto err_enoent;
finish:
  if (folder)
    psync_fstask_release_folder_tasks_locked(folder);
  psync_sql_unlock();
  psync_free(fold_path);
  psync_free(fnew_path);
  return ret;
err_enoent:
  if (folder)
    psync_fstask_release_folder_tasks_locked(folder);
  psync_sql_unlock();
  psync_free(fold_path);
  psync_free(fnew_path);
  debug(D_NOTICE, "returning ENOENT, folder not found");
  return -ENOENT;
}

static int psync_fs_statfs(const char *path, struct statvfs *stbuf){
  uint64_t q, uq;
  debug(D_NOTICE, "statfs %s", path);
/* TODO:
   return -ENOENT if path is invalid if fuse does not call getattr first
   */
  memset(stbuf, 0, sizeof(struct statvfs));
  q=psync_get_uint_value("quota");
  uq=psync_get_uint_value("usedquota");
  stbuf->f_bsize=FS_BLOCK_SIZE;
  stbuf->f_frsize=FS_BLOCK_SIZE;
  stbuf->f_blocks=q/FS_BLOCK_SIZE;
  stbuf->f_bfree=stbuf->f_blocks-uq/FS_BLOCK_SIZE;
  stbuf->f_bavail=stbuf->f_bfree;
  stbuf->f_flag=ST_NOSUID;
  stbuf->f_namemax=1024;
  return 0;
}

static int psync_fs_chmod(const char *path, mode_t mode){
  return 0;
}

int psync_fs_chown(const char *path, uid_t uid, gid_t gid){
  return 0;
}

static int psync_fs_utimens(const char *path, const struct timespec tv[2]){
  return 0;
}

void *psync_fs_init(struct fuse_conn_info *conn){
#if defined(FUSE_CAP_ASYNC_READ)
  conn->want|=FUSE_CAP_ASYNC_READ;
#endif
#if defined(FUSE_CAP_ATOMIC_O_TRUNC)
  conn->want|=FUSE_CAP_ATOMIC_O_TRUNC;
#endif
#if defined(FUSE_CAP_BIG_WRITES)
  conn->want|=FUSE_CAP_BIG_WRITES;
#endif
  conn->max_readahead=0;
  conn->max_write=FS_MAX_WRITE;
  return 0;
}


static struct fuse_operations psync_oper;

#ifdef P_OS_WINDOWS

static int is_mountable(char where){
    DWORD drives = GetLogicalDrives();
    where = tolower(where) - 'a';
    return !(drives & (1<<where));
}

static int get_first_free_drive(){
    DWORD drives = GetLogicalDrives();
    int pos = 3;
    while (pos < 26 && drives & (1<<pos))
        pos ++;
    return pos < 26;
}

static char *psync_fuse_get_mountpoint(){
  const char *stored;
  char *mp = (char*)psync_malloc(3);
  mp[0] = 'P';
  mp[1] = ':';
  mp[2] = '\0';
  stored = psync_setting_get_string(_PS(fsroot));
  if (stored[0] && stored[1] && is_mountable(stored[0])){
      mp[0] = stored[0];
      goto ready;
  }
  if (is_mountable('P')){
      goto ready;
  }
  mp[0] = 'A' + get_first_free_drive();
ready:
  return mp;
}

#else

static char *psync_fuse_get_mountpoint(){
  psync_stat_t st;
  char *mp;
  mp=psync_strdup(psync_setting_get_string(_PS(fsroot)));
  if (psync_stat(mp, &st) && psync_mkdir(mp)){
    psync_free(mp);
    return NULL;
  }
  return mp;
}

#endif

static void psync_fs_do_stop(void){
  debug(D_NOTICE, "stopping");
  pthread_mutex_lock(&start_mutex);
  if (started){
    debug(D_NOTICE, "running fuse_unmount");
    fuse_unmount(psync_current_mountpoint, psync_fuse_channel);
    debug(D_NOTICE, "fuse_unmount exited, running fuse_destroy");
    fuse_destroy(psync_fuse);
    debug(D_NOTICE, "fuse_destroy exited");
    psync_free(psync_current_mountpoint);
    started=0;
    psync_pagecache_flush();
  }
  pthread_mutex_unlock(&start_mutex);
}

void psync_fs_stop(){
  psync_fs_do_stop();
}

#if defined(P_OS_POSIX)

static void psync_signal_handler(int sig){
  debug(D_NOTICE, "got signal %d\n", sig);
  psync_fs_do_stop();
  exit(1);
}

static void psync_set_signal(int sig, void (*handler)(int)){
  struct sigaction sa;
  
  if (unlikely_log(sigaction(sig, NULL, &sa)))
    return;

  if (sa.sa_handler==SIG_DFL){
    memset(&sa, 0, sizeof(struct sigaction));
    sigemptyset(&(sa.sa_mask));
    sa.sa_handler=handler;
    sa.sa_flags=0;
    sigaction(sig, &sa, NULL);
  }
}

static void psync_setup_signals(){
  psync_set_signal(SIGTERM, psync_signal_handler);
  psync_set_signal(SIGINT, psync_signal_handler);
  psync_set_signal(SIGHUP, psync_signal_handler);
}

#endif

static void psync_fs_init_once(){
  psync_fstask_init();
  psync_pagecache_init();
  atexit(psync_fs_do_stop);
#if defined(P_OS_POSIX)
  psync_setup_signals();
#endif
}

static void psync_fuse_thread(){
  pthread_mutex_lock(&start_mutex);
  if (!initonce){
    psync_fs_init_once();
    initonce=1;
  }
  pthread_mutex_unlock(&start_mutex);
  debug(D_NOTICE, "running fuse_loop_mt");
  fuse_loop_mt(psync_fuse);
  debug(D_NOTICE, "fuse_loop_mt exited");
}

int psync_fs_start(){
  char *mp;
  struct fuse_args args=FUSE_ARGS_INIT(0, NULL);

#if defined(P_OS_LINUX)
  fuse_opt_add_arg(&args, "-oallow_root");
  fuse_opt_add_arg(&args, "-oauto_unmount");
  fuse_opt_add_arg(&args, "-ofsname=pCloud.fs");
#endif
#if defined(P_OS_MACOSX)
  fuse_opt_add_arg(&args, "-ovolname=pCloudDrive");
  fuse_opt_add_arg(&args, "-olocal");
#endif

  psync_oper.init     = psync_fs_init;
  psync_oper.getattr  = psync_fs_getattr;
  psync_oper.readdir  = psync_fs_readdir;
  psync_oper.open     = psync_fs_open;
  psync_oper.create   = psync_fs_creat;
  psync_oper.release  = psync_fs_release;
  psync_oper.flush    = psync_fs_flush;
  psync_oper.fsync    = psync_fs_fsync;
  psync_oper.fsyncdir = psync_fs_fsyncdir;
  psync_oper.read     = psync_fs_read;
  psync_oper.write    = psync_fs_write;
  psync_oper.mkdir    = psync_fs_mkdir;
  psync_oper.rmdir    = psync_fs_rmdir;
  psync_oper.unlink   = psync_fs_unlink;
  psync_oper.rename   = psync_fs_rename;
  psync_oper.statfs   = psync_fs_statfs;
  psync_oper.chmod    = psync_fs_chmod;
  psync_oper.chown    = psync_fs_chown;
  psync_oper.utimens  = psync_fs_utimens;

#if defined(P_OS_POSIX)
  myuid=getuid();
  mygid=getgid();
#endif
  pthread_mutex_lock(&start_mutex);
  if (started)
    goto err00;
  mp=psync_fuse_get_mountpoint();
  psync_fuse_channel=fuse_mount(mp, &args);
  if (unlikely_log(!psync_fuse_channel))
    goto err0;
  psync_fuse=fuse_new(psync_fuse_channel, &args, &psync_oper, sizeof(psync_oper), NULL);
  if (unlikely_log(!psync_fuse))
    goto err1;
  psync_current_mountpoint=mp;
  started=1;
  pthread_mutex_unlock(&start_mutex);
  psync_run_thread("fuse", psync_fuse_thread);
  return 0;
err1:
  fuse_unmount(mp, psync_fuse_channel);
err0:
  psync_free(mp);
err00:
  pthread_mutex_unlock(&start_mutex);
  return -1;
}

int psync_fs_isstarted(){
  int s;
  pthread_mutex_lock(&start_mutex);
  s=started;
  pthread_mutex_unlock(&start_mutex);
  return s;
}

int psync_fs_remount(){
  int s;
  pthread_mutex_lock(&start_mutex);
  s=started;
  pthread_mutex_unlock(&start_mutex);
  if (s){
    psync_fs_stop();
    return psync_fs_start();
  }
  else
    return 0;
}
