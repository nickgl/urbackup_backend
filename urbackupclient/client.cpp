/*************************************************************************
*    UrBackup - Client/Server backup system
*    Copyright (C) 2011-2016 Martin Raiber
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU Affero General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include "client.h"
#include "../Interface/Server.h"
#include "../Interface/File.h"
#include "../Interface/SettingsReader.h"
#ifdef _WIN32
#include "DirectoryWatcherThread.h"
#else
#include <errno.h>
#endif
#include "../stringtools.h"
#include "../common/data.h"
#include "../md5.h"
#include "database.h"
#include "ServerIdentityMgr.h"
#include "ClientService.h"
#include "../urbackupcommon/sha2/sha2.h"
#include <algorithm>
#include <fstream>
#include <stdlib.h>
#include "file_permissions.h"
#include <memory>
#include <assert.h>
#include "../urbackupcommon/chunk_hasher.h"
#include "../urbackupcommon/TreeHash.h"
#include "../fileservplugin/chunk_settings.h"
#include "ImageThread.h"
#include "../common/adler32.h"

//For truncating files
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys\stat.h>
#include "win_disk_mon.h"
#include "win_all_volumes.h"
#else
#include "../config.h"
#ifdef HAVE_MNTENT_H
#include <mntent.h>
#endif
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif
#include <set>
#include "../urbackupcommon/glob.h"
#include "TokenCallback.h"

volatile bool IdleCheckerThread::idle=false;
volatile bool IdleCheckerThread::pause=false;
volatile bool IndexThread::stop_index=false;
std::map<std::string, std::string> IndexThread::filesrv_share_dirs;
IMutex* IndexThread::cbt_shadow_id_mutex;
std::map<std::string, int> IndexThread::cbt_shadow_ids;

const char IndexThread::IndexThreadAction_StartFullFileBackup=0;
const char IndexThread::IndexThreadAction_StartIncrFileBackup=1;
const char IndexThread::IndexThreadAction_CreateShadowcopy = 2;
const char IndexThread::IndexThreadAction_ReferenceShadowcopy = 11;
const char IndexThread::IndexThreadAction_ReleaseShadowcopy = 3;
const char IndexThread::IndexThreadAction_GetLog=9;
const char IndexThread::IndexThreadAction_PingShadowCopy=10;
const char IndexThread::IndexThreadAction_AddWatchdir = 5;
const char IndexThread::IndexThreadAction_RemoveWatchdir = 6;
const char IndexThread::IndexThreadAction_UpdateCbt = 7;
const char IndexThread::IndexThreadAction_SnapshotCbt = 12;

extern PLUGIN_ID filesrv_pluginid;

namespace
{
	const int64 idletime = 60000;
	const unsigned int nonidlesleeptime = 500;
	const unsigned short tcpport = 35621;
	const unsigned short udpport = 35622;
	const unsigned int shadowcopy_timeout = 7 * 24 * 60 * 60 * 1000;
	const unsigned int shadowcopy_startnew_timeout = 55 * 60 * 1000;
	const size_t max_file_buffer_size = 4 * 1024 * 1024;
	const int64 file_buffer_commit_interval = 120 * 1000;
}


void IdleCheckerThread::operator()(void)
{
	int lx,ly;
	int x,y;
	getMousePos(x,y);
	lx=x;
	ly=y;

	int64 last_move=Server->getTimeMS();

	while(true)
	{
		Server->wait(1000);
		getMousePos(x,y);
		if(x!=lx || y!=ly )
		{
			lx=x;
			ly=y;
			last_move=Server->getTimeMS();
			idle=false;
		}
		else if(Server->getTimeMS()-last_move>idletime)
		{
			idle=true;
		}
	}
}

bool IdleCheckerThread::getIdle(void)
{
	return true;//idle;
}

bool IdleCheckerThread::getPause(void)
{
	return pause;
}

void IdleCheckerThread::setPause(bool b)
{
	pause=b;
}

namespace
{
#ifdef _WIN32

	struct ON_DISK_USN_JOURNAL_DATA
	{
		uint64 MaximumSize; 
		uint64 AllocationDelta;
		uint64 UsnJournalID;
		int64 LowestValidUsn;
	};

	int64 getUsnNum( const std::string& dir, int64& sequence_id )
	{
		WCHAR volume_path[MAX_PATH]; 
		BOOL ok = GetVolumePathNameW(Server->ConvertToWchar(dir).c_str(), volume_path, MAX_PATH);
		if(!ok)
		{
			Server->Log("GetVolumePathName(dir, volume_path, MAX_PATH) failed in getUsnNum", LL_ERROR);
			return -1;
		}

		std::string vol=Server->ConvertFromWchar(volume_path);

		if(vol.size()>0)
		{
			if(vol[vol.size()-1]=='\\')
			{
				vol.erase(vol.size()-1,1);
			}
		}

		if(!vol.empty() && vol[0]!='\\')
		{
			vol = "\\\\.\\"+vol;
		}

		HANDLE hVolume=CreateFileW(Server->ConvertToWchar(vol).c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if(hVolume==INVALID_HANDLE_VALUE)
		{
			Server->Log("CreateFile of volume '"+vol+"' failed. - getUsnNum", LL_ERROR);
			return -1;
		}

		USN_JOURNAL_DATA data;
		DWORD r_bytes;
		BOOL b=DeviceIoControl(hVolume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &data, sizeof(USN_JOURNAL_DATA), &r_bytes, NULL);

		CloseHandle(hVolume);

		if(b)
		{
			sequence_id=data.UsnJournalID;
			return data.NextUsn;
		}
		else
		{
			std::auto_ptr<IFile> journal_info(Server->openFile(vol+"\\$Extend\\$UsnJrnl:$Max", MODE_READ_SEQUENTIAL_BACKUP));

			if(journal_info.get()==NULL) return -1;

			ON_DISK_USN_JOURNAL_DATA journal_data = {};
			if(journal_info->Read(reinterpret_cast<char*>(&journal_data), sizeof(journal_data))!=sizeof(journal_data))
			{
				return -1;
			}

			sequence_id = journal_data.UsnJournalID;

			std::auto_ptr<IFile> journal(Server->openFile(vol+"\\$Extend\\$UsnJrnl:$J", MODE_READ_SEQUENTIAL_BACKUP));

			if(journal.get()==NULL) return -1;

			return journal->Size();
		}
	}
#endif

#ifndef _WIN32
	std::string getFolderMount(const std::string& path)
	{		
#ifndef HAVE_MNTENT_H
		return std::string();
#else
		FILE *aFile;

		aFile = setmntent("/proc/mounts", "r");
		if (aFile == NULL) {
			return std::string();
		}
		struct mntent *ent;
		std::string maxmount;
		while (NULL != (ent = getmntent(aFile)))
		{
			if(path.find(ent->mnt_dir)==0 &&
				std::string(ent->mnt_dir).size()>maxmount.size())
			{
				maxmount = ent->mnt_dir;
			}
		}
		endmntent(aFile);

		return maxmount;
#endif //HAVE_MNTENT_H
	}
#endif //!_WIN32
}

IMutex *IndexThread::filelist_mutex=NULL;
IPipe* IndexThread::msgpipe=NULL;
IFileServ *IndexThread::filesrv=NULL;
IMutex *IndexThread::filesrv_mutex=NULL;

std::string add_trailing_slash(const std::string &strDirName)
{
	if(!strDirName.empty() && strDirName[strDirName.size()-1]!=os_file_sep()[0])
	{
		return strDirName+os_file_sep();
	}
	else if(strDirName.empty())
	{
		return os_file_sep();
	}
	else
	{
		return strDirName;
	}
}

IndexThread::IndexThread(void)
	: index_error(false), last_filebackup_filetime(0), index_group(-1),
	with_scripts(false), volumes_cache(NULL), phash_queue(NULL)
{
	if(filelist_mutex==NULL)
		filelist_mutex=Server->createMutex();
	if(msgpipe==NULL)
		msgpipe=Server->createMemoryPipe();
	if(filesrv_mutex==NULL)
		filesrv_mutex=Server->createMutex();

	read_error_mutex = Server->createMutex();

	if (cbt_shadow_id_mutex == NULL)
		cbt_shadow_id_mutex = Server->createMutex();

	contractor=NULL;

	dwt=NULL;

	if(Server->getPlugin(Server->getThreadID(), filesrv_pluginid))
	{
		start_filesrv();
	}
	else
	{
		filesrv=NULL;
		Server->Log("Error starting fileserver", LL_ERROR);
	}

	modify_file_buffer_size=0;
	end_to_end_file_backup_verification=false;
	add_file_buffer_size=0;
	calculate_filehashes_on_client=false;
	last_tmp_update_time=0;
	last_file_buffer_commit_time=0;
}

IndexThread::~IndexThread()
{
	filesrv->stopServer();

#ifdef _WIN32
	if(dwt!=NULL)
	{
		dwt->stop();
		Server->getThreadPool()->waitFor(dwt_ticket);
		delete dwt;
	}
#endif

	((IFileServFactory*)(Server->getPlugin(Server->getThreadID(), filesrv_pluginid)))->destroyFileServ(filesrv);
	Server->destroy(filelist_mutex);
	Server->destroy(msgpipe);
	Server->destroy(filesrv_mutex);
	Server->destroy(cbt_shadow_id_mutex);
	Server->destroy(read_error_mutex);
	cd->destroyQueries();
	delete cd;
}

IMutex* IndexThread::getFilelistMutex(void)
{
	return filelist_mutex;
}

void IndexThread::updateDirs(void)
{
	readBackupDirs();
	readSnapshotGroups();

#ifdef _WIN32
	std::vector<std::string> watching;
	std::vector<ContinuousWatchEnqueue::SWatchItem> continuous_watch;
	for(size_t i=0;i<backup_dirs.size();++i)
	{
		watching.push_back(backup_dirs[i].path);

		if(backup_dirs[i].group==c_group_continuous)
		{
			continuous_watch.push_back(
				ContinuousWatchEnqueue::SWatchItem(backup_dirs[i].path, backup_dirs[i].tname));
		}
	}

	if(dwt==NULL)
	{
		dwt=new DirectoryWatcherThread(watching, continuous_watch);
		dwt_ticket=Server->getThreadPool()->execute(dwt, "directory watcher");
	}
	else
	{
		for(size_t i=0;i<backup_dirs.size();++i)
		{
			std::string msg="A"+backup_dirs[i].path;
			dwt->getPipe()->Write(msg);
		}
	}
#endif
}

void IndexThread::log_read_errors(const std::string& share_name, const std::string& orig_path)
{
	IScopedLock lock2(read_error_mutex);

	for (size_t i = 0; i < read_errors.size();)
	{
		if (read_errors[i].sharename == share_name)
		{
			VSSLog("There was a read error during the last file backup while backing up the file \"" + read_errors[i].filepath + "\" at position " + convert(read_errors[i].filepos)
				+ " in backup path \""+ orig_path +"\" ("+read_errors[i].msg+"). "
				+ "This might have prevented the backup from finishing. "
				"If this keeps occuring, please have a look at the system error log and at the disk S.M.A.R.T. values.", LL_WARNING);

			read_errors.erase(read_errors.begin() + i);
		}
		else
		{
			++i;
		}
	}
}

void IndexThread::operator()(void)
{
	Server->waitForStartupComplete();

#ifdef _WIN32
	initVss();
#endif

	if(backgroundBackupsEnabled(std::string()))
	{
#ifndef _DEBUG
		background_prio.reset(new ScopedBackgroundPrio());
#endif
	}
	

	db=Server->getDatabase(Server->getThreadID(), URBACKUPDB_CLIENT);
	cd=new ClientDAO(Server->getDatabase(Server->getThreadID(), URBACKUPDB_CLIENT));

#ifdef _WIN32
#ifdef ENABLE_VSS
	cleanup_saved_shadowcopies();
#endif
#endif
	
	updateDirs();
	register_token_callback();

	updateCbt();

	std::string last_index;
	bool async_timeout = false;
	int64 async_timeout_starttime = 0;

	while(true)
	{
		std::string msg;
		if(contractor!=NULL)
		{
			while(msg!="exit"
				&& (!async_timeout
					|| Server->getTimeMS() - async_timeout_starttime < async_index_timeout_with_grace) )
			{
				contractor->Read(&msg);
				if(msg!="exit")
				{
					contractor->Write(msg);
					Server->wait(100);
				}
			}
			Server->destroy(contractor);
			contractor=NULL;
		}
		msgpipe->Read(&msg);

		async_timeout = false;
		CRData data(&msg);
		char action;
		data.getChar(&action);
		data.getVoidPtr((void**)&contractor);
		if(action==IndexThreadAction_StartIncrFileBackup
			|| ( (last_index=="full" || last_index=="vfull") && action == IndexThreadAction_StartFullFileBackup ) )
		{
			Server->Log("Removing VSS log data...", LL_DEBUG);
			vsslog.clear();

			if (action == IndexThreadAction_StartFullFileBackup)
			{
				if (last_index == "full")
				{
					VSSLog("Last full index unfinished. Performing incremental (virtual full) indexing...", LL_INFO);
				}
				else
				{
					VSSLog("Last virtual full index unfinished. Performing incremental (virtual full) indexing...", LL_INFO);
				}
			}

			data.getStr(&starttoken);
			data.getInt(&index_group);
			unsigned int flags;
			data.getUInt(&flags);
			data.getStr(&index_clientsubname);
			data.getInt(&sha_version);
			int running_jobs = 2;
			data.getInt(&running_jobs);
			char async_index = 0;
			data.getChar(&async_index);
			std::string async_ticket;
			data.getStr2(&async_ticket);

			if (async_index == 1)
			{
				async_timeout = true;
			}

			if (!async_ticket.empty())
			{
				initParallelHashing(async_ticket);
				contractor->Write("phash");
			}

			setFlags(flags);

			//incr backup
			bool has_dirs = readBackupDirs();
			bool has_scripts = readBackupScripts(action == IndexThreadAction_StartFullFileBackup);
			if( !has_dirs && !has_scripts )
			{
				contractor->Write("no backup dirs");

				if (async_timeout)
				{
					async_timeout_starttime = Server->getTimeMS();
				}

				continue;
			}
#ifdef _WIN32
			if(cd->hasChangedGap())
			{
				Server->Log("Deleting file-index... GAP found...", LL_INFO);

				std::vector<std::string> gaps=cd->getGapDirs();

				std::string q_str="DELETE FROM files WHERE (tgroup=0 OR tgroup=?)";
				if(!gaps.empty())
				{
					q_str+=" AND (";
				}
				for(size_t i=0;i<gaps.size();++i)
				{
					q_str+="name GLOB ?";
					if(i+1<gaps.size())
						q_str+=" OR ";
				}
				if (!gaps.empty())
				{
					q_str += ")";
				}

				IQuery *q=db->Prepare(q_str, false);
				q->Bind(index_group+1);
				for(size_t i=0;i<gaps.size();++i)
				{
					Server->Log("Deleting file-index from drive \""+gaps[i]+"\"", LL_INFO);
					q->Bind(gaps[i]+"*");
				}

				q->Write();
				q->Reset();
				db->destroyQuery(q);

				if(dwt!=NULL)
				{
					dwt->stop();
					Server->getThreadPool()->waitFor(dwt_ticket);
					delete dwt;
					dwt=NULL;
					updateDirs();
				}
			}
#endif
			monitor_disk_failures();

			int e_rc;
			if ( (e_rc=execute_prebackup_hook(true, starttoken, index_group))!=0 )
			{
				contractor->Write("error - prefilebackup script failed with error code "+convert(e_rc));
			}
			else if(!stop_index)
			{
				if (action == IndexThreadAction_StartIncrFileBackup)
				{
					last_index = "incr";
				}
				else
				{
					last_index = "vfull";
				}

				indexDirs(false, running_jobs>1);

				if ( (e_rc=execute_postindex_hook(true, starttoken, index_group))!=0 )
				{
					contractor->Write("error - postfileindex script failed with error code " + convert(e_rc));
				}
				else if(stop_index)
				{
					contractor->Write("error - stopped indexing 2");
				}
				else if(index_error)
				{
					contractor->Write("error - index error");
				}
				else
				{
					contractor->Write("done");
				}
			}
			else
			{
				contractor->Write("error - stop_index 1");
			}

			if (async_timeout)
			{
				async_timeout_starttime = Server->getTimeMS();
			}
		}
		else if(action==IndexThreadAction_StartFullFileBackup)
		{
			Server->Log("Removing VSS log data...", LL_DEBUG);
			vsslog.clear();

			data.getStr(&starttoken);
			data.getInt(&index_group);
			unsigned int flags;
			data.getUInt(&flags);
			data.getStr(&index_clientsubname);
			data.getInt(&sha_version);
			int running_jobs = 2;
			data.getInt(&running_jobs);
			char async_index = 0;
			data.getChar(&async_index);
			std::string async_ticket;
			data.getStr2(&async_ticket);

			if (async_index == 1)
			{
				async_timeout = true;
			}

			if (!async_ticket.empty())
			{
				initParallelHashing(async_ticket);
				contractor->Write("phash");
			}

			setFlags(flags);

			bool has_dirs = readBackupDirs();
			bool has_scripts = readBackupScripts(true);
			if(!has_dirs && !has_scripts)
			{
				contractor->Write("no backup dirs");

				if (async_timeout)
				{
					async_timeout_starttime = Server->getTimeMS();
				}

				continue;
			}
			//full backup
			{
				Server->Log("Deleting files... doing full index...", LL_INFO);
				resetFileEntries();
			}

			monitor_disk_failures();

			int e_rc;
			if ( (e_rc=execute_prebackup_hook(false, starttoken, index_group))!=0 )
			{
				contractor->Write("error - prefilebackup script failed with error code "+convert(e_rc));
			}
			else
			{
				last_index = "full";

				indexDirs(true, running_jobs>1);

				if ( (e_rc=execute_postindex_hook(false, starttoken, index_group))!=0 )
				{
					contractor->Write("error - postfileindex script failed with error code "+convert(e_rc));
				}
				else if (stop_index)
				{
					contractor->Write("error - stopped indexing");
				}
				else if (index_error)
				{
					contractor->Write("error - index error");
				}
				else
				{
					contractor->Write("done");
				}
			}

			if (async_timeout)
			{
				async_timeout_starttime = Server->getTimeMS();
			}
		}
		else if(action==IndexThreadAction_CreateShadowcopy
			 || action==IndexThreadAction_ReferenceShadowcopy )
		{
			vsslog.clear();
			if (action == IndexThreadAction_CreateShadowcopy)
			{
				readSnapshotGroups();
			}

			std::string scdir;
			data.getStr(&scdir);
			data.getStr(&starttoken);
			uchar image_backup=0;
			data.getUChar(&image_backup);
			unsigned char fileserv;
			bool hfs=data.getUChar(&fileserv);

			int running_jobs = 2;
			index_clientsubname.clear();
			if (hfs)
			{
				data.getStr(&index_clientsubname);
				data.getInt(&running_jobs);
			}

#ifdef _WIN32
			if(action==IndexThreadAction_ReferenceShadowcopy
				&& image_backup==0
				&& (scdir == "windows_components" || scdir == "windows_components_config") )
			{
				contractor->Write("done--");
				continue;
			}
#endif

			if (image_backup != 0)
			{
				int rc = execute_preimagebackup_hook(image_backup == 2, starttoken);

				if (rc != 0)
				{
					VSSLog("Pre image backup hook failed with error code " + convert(rc), LL_ERROR);
					contractor->Write("failed");
					continue;
				}
			}

			bool reference_sc = action == IndexThreadAction_ReferenceShadowcopy;

			SCDirs *scd = getSCDir(scdir, index_clientsubname, image_backup!=0);
			
			if(scd->running==true && Server->getTimeSeconds()-scd->starttime<shadowcopy_timeout/1000)
			{
				if(scd->ref!=NULL && image_backup==0)
				{
					scd->ref->dontincrement=true;
				}
				bool onlyref = reference_sc;
				if(start_shadowcopy(scd, &onlyref, image_backup!=0?true:false, running_jobs>1, std::vector<SCRef*>(), image_backup!=0?true:false))
				{
					if (scd->ref!=NULL
						&& !onlyref)
					{
						for (size_t k = 0; k < sc_refs.size(); ++k)
						{
							if (sc_refs[k]->ssetid == scd->ref->ssetid)
							{
								if (sc_refs[k]->cbt)
								{
									sc_refs[k]->cbt = finishCbt(sc_refs[k]->target,
										image_backup != 0 ? sc_refs[k]->save_id : -1, sc_refs[k]->volpath,
										image_backup != 0);
								}
							}
						}
					}

					if (scd->ref != NULL
						&& !scd->ref->cbt
						&& !disableCbt(scd->orig_target) )
					{
						VSSLog("Error disabling change block tracking for " + scd->orig_target, LL_ERROR);
						contractor->Write("failed");
					}
					else
					{
						contractor->Write("done-" + convert(scd->ref->save_id) + "-" + scd->target + otherVolumeInfo(scd, onlyref));
					}
				}
				else
				{
					if (!disableCbt(scd->orig_target))
					{
						VSSLog("Error disabling change block tracking for " + scd->orig_target+" (2)", LL_ERROR);
					}
					VSSLog("Getting shadowcopy of \""+scd->dir+"\" failed.", LL_ERROR);
					contractor->Write("failed");
				}
			}
			else
			{
				if(scd->running==true)
				{
					Server->Log("Removing shadowcopy \""+scd->dir+"\" because of timeout...", LL_WARNING);
					bool b=release_shadowcopy(scd, false, -1, scd);
					if(!b)
					{
						Server->Log("Deleting shadowcopy of \""+scd->dir+"\" failed.", LL_ERROR);
					}
				}

				scd->dir=scdir;
				scd->starttime=Server->getTimeSeconds();
				if(hfs && fileserv==0)
				{
					scd->target=scd->dir;
					scd->fileserv=false;
				}
				else
				{
					scd->target=getShareDir(scd->dir);
					scd->fileserv=true;
				}

				scd->orig_target=scd->target;

				Server->Log("Creating shadowcopy of \""+scd->dir+"\"...", LL_DEBUG);
				bool onlyref = reference_sc;
				bool b= start_shadowcopy(scd, &onlyref, image_backup!=0?true:false, running_jobs>1, std::vector<SCRef*>(), image_backup==0?false:true);
				Server->Log("done.", LL_DEBUG);
				if(!b || scd->ref==NULL)
				{
					if(scd->fileserv)
					{
						shareDir(std::string(), scd->dir, scd->target);
					}

					if (!disableCbt(scd->orig_target))
					{
						VSSLog("Error disabling change block tracking for " + scd->orig_target + " (3)", LL_ERROR);
					}

					contractor->Write("failed");
					Server->Log("Creating shadowcopy of \""+scd->dir+"\" failed.", LL_ERROR);
				}
				else
				{
					if (scd->ref != NULL
						&& !onlyref)
					{
						for (size_t k = 0; k < sc_refs.size(); ++k)
						{
							if (sc_refs[k]->ssetid == scd->ref->ssetid)
							{
								if (sc_refs[k]->cbt)
								{
									sc_refs[k]->cbt = finishCbt(sc_refs[k]->target, 
										image_backup != 0 ? sc_refs[k]->save_id : -1, sc_refs[k]->volpath,
										image_backup != 0);
								}
							}
						}
					}

					if (scd->ref != NULL
						&& !scd->ref->cbt
						&& !disableCbt(scd->orig_target))
					{
						VSSLog("Error disabling change block tracking for " + scd->orig_target, LL_ERROR);

						if (scd->fileserv)
						{
							shareDir(starttoken, scd->dir, scd->target);
						}

						contractor->Write("failed");
					}
					else
					{
						contractor->Write("done-" + convert(scd->ref->save_id) + "-" + scd->target + otherVolumeInfo(scd, onlyref));
						scd->running = true;
					}

					if ((image_backup != 0 && FileExists("create_md5sums_imagebackup"))
						|| (image_backup == 0 && FileExists("create_md5sums_filebackup")))
					{
						createMd5sumsFile(removeDirectorySeparatorAtEnd(scd->target), scd->orig_target);
					}
				}
			}
		}
		else if(action==IndexThreadAction_ReleaseShadowcopy) // remove shadowcopy
		{
			vsslog.clear();

			std::string scdir;
			data.getStr(&scdir);
			data.getStr(&starttoken);
			uchar image_backup=0;
			data.getUChar(&image_backup);

			int save_id=-1;
			data.getInt(&save_id);
			index_clientsubname.clear();
			data.getStr(&index_clientsubname);
			int issues = 0;
			data.getInt(&issues);

#ifdef _WIN32
			if (image_backup == 0
				&& (scdir == "windows_components" || scdir == "windows_components_config"))
			{
				contractor->Write("done");
				continue;
			}
#endif		

			int64 starttime = Server->getTimeMS();

			while (filesrv != NULL
				&& filesrv->hasActiveTransfers(scdir, starttoken)
				&& Server->getTimeMS() - starttime < 5000)
			{
				Server->wait(100);
			}

			if (filesrv != NULL
				&& filesrv->hasActiveTransfers(scdir, starttoken) )
			{
				contractor->Write("in use");
			}
			else
			{
				bool del_error = false;
#ifdef _WIN32
				std::map<std::string, SVssInstance*>::iterator it_inst = vss_name_instances.find(starttoken + "|" + scdir);
				if (it_inst != vss_name_instances.end())
				{
					--it_inst->second->refcount;
					it_inst->second->issues += issues;

					for (size_t i = 0; i < it_inst->second->parents.size(); ++i)
					{
						it_inst->second->parents[i]->issues += issues;
					}

					if (it_inst->second->refcount == 0)
					{
						if(it_inst->second->set_succeeded)
						{
							HRESULT hr = it_inst->second->backupcom->SetBackupSucceeded(it_inst->second->instanceId,
								it_inst->second->writerId, it_inst->second->componentType,
								it_inst->second->logicalPath.empty() ? NULL : Server->ConvertToWchar(it_inst->second->logicalPath).c_str(),
								Server->ConvertToWchar(it_inst->second->componentName).c_str(),
								it_inst->second->issues == 0 ? true : false);

							if (hr != S_OK)
							{
								VSSLog("Error setting component \"" + it_inst->second->componentName + "\" with logical path \"" + it_inst->second->logicalPath +
									"\" to succeeded. VSS error code " + GetErrorHResErrStr(hr), LL_ERROR);
								del_error = true;
							}
						}

						for (size_t i = 0; i < it_inst->second->parents.size(); ++i)
						{
							SVssInstance* parent = it_inst->second->parents[i];

							assert(parent->refcount > 0);
							--parent->refcount;

							if (parent->refcount == 0)
							{
								if(parent->set_succeeded)
								{
									HRESULT hr = parent->backupcom->SetBackupSucceeded(parent->instanceId,
										parent->writerId, parent->componentType,
										parent->logicalPath.empty() ? NULL : Server->ConvertToWchar(parent->logicalPath).c_str(),
										Server->ConvertToWchar(parent->componentName).c_str(),
										parent->issues == 0 ? true : false);

									if (hr != S_OK)
									{
										VSSLog("Error setting component \"" + parent->componentName + "\" with logical path \"" + parent->logicalPath +
											"\" to succeeded. VSS error code " + GetErrorHResErrStr(hr), LL_ERROR);
										del_error = true;
									}
								}

								delete parent;
							}
						}

						delete it_inst->second;
					}

					vss_name_instances.erase(it_inst);
				}
#endif

				SCDirs *scd = getSCDir(scdir, index_clientsubname, image_backup!=0);
				if (scd->running == false)
				{
					if (!release_shadowcopy(scd, image_backup != 0 ? true : false, save_id))
					{
						Server->Log("Invalid action -- Creating shadow copy failed?", LL_ERROR);
						contractor->Write("failed");
					}
					else
					{
						contractor->Write(del_error ? "failed" : "done");
					}
				}
				else
				{
					std::string release_dir = scd->dir;
					bool b = release_shadowcopy(scd, image_backup != 0 ? true : false, save_id);
					if (!b)
					{
						contractor->Write("failed");
						Server->Log("Deleting shadowcopy of \"" + release_dir + "\" failed.", LL_ERROR);
					}
					else
					{
						contractor->Write(del_error ? "failed" : "done");
					}
				}
			}
		}
		else if(action==4) //lookup shadowdrive path
		{
			int save_id;
			if(data.getInt(&save_id))
			{
				std::string path=lookup_shadowcopy(save_id);
				if(path.empty())
				{
					contractor->Write("failed");
				}
				else
				{
					for (size_t i = 0; i < sc_refs.size(); ++i)
					{
						if (sc_refs[i]->save_id == save_id)
						{
							sc_refs[i]->starttime = Server->getTimeSeconds();
						}
					}

					cd->updateShadowCopyStarttime(save_id);

					contractor->Write("done-"+convert(save_id)+"-"+path);
				}
			}
		}
#ifdef _WIN32
		else if(action==IndexThreadAction_AddWatchdir)
		{
			std::string dir;
			if(data.getStr(&dir))
			{
				std::string msg="A"+os_get_final_path(dir);
				dwt->getPipe()->Write(msg);
			}
			std::string name;
			if(data.getStr(&name))
			{
				std::string msg="C"+os_get_final_path(dir)+"|"+(name);
				dwt->getPipe()->Write(msg);
			}
			contractor->Write("done");
			stop_index=false;
		}
		else if(action==IndexThreadAction_RemoveWatchdir)
		{
			std::string dir;
			if(data.getStr(&dir))
			{
				std::string msg="D"+os_get_final_path(dir);
				dwt->getPipe()->Write(msg);
			}
			std::string name;
			if(data.getStr(&name))
			{
				std::string msg="X"+os_get_final_path(dir)+"|"+(name);
				dwt->getPipe()->Write(msg);
			}
			contractor->Write("done");
			stop_index=false;
		}
#endif
		else if(action==7) // restart filesrv
		{
			IScopedLock lock(filesrv_mutex);
			filesrv->stopServer();
			start_filesrv();
			readBackupDirs();
		}
		else if(action==8) //stop
		{
			break;
		}
		else if(action==IndexThreadAction_GetLog)
		{
			if (last_index == "full")
			{
				last_index = "full_done";
			}
			else if (last_index == "incr")
			{
				last_index = "incr_done";
			}
			else if (last_index == "vfull")
			{
				last_index = "vfull_done";
			}

			std::string ret;
			ret+="0-"+convert(Server->getTimeSeconds())+"-\n";
			for(size_t i=0;i<vsslog.size();++i)
			{
				ret+=convert(vsslog[i].loglevel)+"-"+convert(vsslog[i].times)+"-"+vsslog[i].msg+"\n";
			}
			Server->Log("VSS logdata - "+convert(ret.size())+" bytes", LL_DEBUG);
			contractor->Write(ret);
		}
		else if(action==IndexThreadAction_PingShadowCopy)
		{
			std::string scdir;
			data.getStr(&scdir);
			int save_id=-1;
			data.getInt(&save_id);
			index_clientsubname.clear();
			data.getStr(&index_clientsubname);

			SCDirs *scd = getSCDir(scdir, index_clientsubname, true);

			if(scd!=NULL && scd->ref!=NULL)
			{
				scd->ref->starttime = Server->getTimeSeconds();
			}
			if (scd != NULL)
			{
				scd->starttime = Server->getTimeSeconds();
			}

			if(save_id!=-1)
			{
				cd->updateShadowCopyStarttime(save_id);
			}
		}
		else if (action == IndexThreadAction_UpdateCbt)
		{
			updateCbt();
		}
		else if (action == IndexThreadAction_SnapshotCbt)
		{
			std::string volume;
			data.getStr(&volume);

			if (prepareCbt(volume)
				&& finishCbt(volume, -1, std::string(), false))
			{
				contractor->Write("done");
			}
			else
			{
				contractor->Write("failed");
			}
		}
	}

	delete this;
}

void IndexThread::indexDirs(bool full_backup, bool simultaneous_other)
{
	readPatterns(index_group, index_clientsubname,
		index_exclude_dirs, index_include_dirs);
	file_id = 0;

	updateDirs();

	writeTokens();

	token_cache.reset();

#ifdef _WIN32
	getVssSettings();

	bool backup_with_vss_components = index_group==0 
		&& (vss_select_all_components || !vss_select_components.empty());
#else
	bool backup_with_vss_components = false;
#endif

	index_follow_last = false;
	index_keep_files = false;

	std::vector<std::string> selected_dirs;
	std::vector<int> selected_dir_db_tgroup;
	for(size_t i=0;i<backup_dirs.size();++i)
	{
		if(backup_dirs[i].group==index_group
			|| (backup_with_vss_components
				&& backup_dirs[i].group== c_group_vss_components) )
		{
			selected_dirs.push_back(removeDirectorySeparatorAtEnd(backup_dirs[i].path));
#ifdef _WIN32
			selected_dirs[selected_dirs.size()-1]=strlower(selected_dirs[selected_dirs.size()-1]);
#endif
			if (backup_dirs[i].flags & EBackupDirFlag_ShareHashes)
			{
				selected_dir_db_tgroup.push_back(0);
			}
			else
			{
				selected_dir_db_tgroup.push_back(index_group + 1);
			}

			if (backup_dirs[i].flags & EBackupDirFlag_KeepFiles
				&& !full_backup
				&& !backup_dirs[i].reset_keep)
			{
				index_follow_last = true;
			}
		}
	}

#ifdef _WIN32
	//Invalidate cache
	DirectoryWatcherThread::freeze();
	DirectoryWatcherThread::update_and_wait(open_files);

	changed_dirs.clear();
	for(size_t i=0;i<selected_dirs.size();++i)
	{
		std::vector<std::string> acd=cd->getChangedDirs(selected_dirs[i], true);
		changed_dirs.insert(changed_dirs.end(), acd.begin(), acd.end() );
		DirectoryWatcherThread::reset_mdirs(selected_dirs[i]);
	}

	//move GAP dirs to backup table
	cd->getChangedDirs("##-GAP-##", true);
	DirectoryWatcherThread::reset_mdirs("##-GAP-##");
	
	for(size_t i=0;i<selected_dirs.size();++i)
	{
		std::vector<std::string> deldirs=cd->getDelDirs(selected_dirs[i]);
		VSSLog("Removing deleted directories from index...", LL_DEBUG);
		for(size_t j=0;j<deldirs.size();++j)
		{
			cd->removeDeletedDir(deldirs[j], selected_dir_db_tgroup[i]);
		}
	}

	std::string tmp = cd->getMiscValue("last_filebackup_filetime_lower");
	if(!tmp.empty())
	{
		last_filebackup_filetime = watoi64(tmp);
	}
	else
	{
		last_filebackup_filetime = 0;
	}

	_i64 last_filebackup_filetime_new = DirectoryWatcherThread::get_current_filetime();
#endif

	bool has_stale_shadowcopy=false;
	bool has_active_transaction = false;

	std::sort(changed_dirs.begin(), changed_dirs.end());


	for(size_t i=0;i<backup_dirs.size();++i)
	{
		backup_dirs[i].symlinked_confirmed=false;
	}

	std::vector<SCRef*> past_refs;

	last_tmp_update_time=Server->getTimeMS();
	index_error=false;

	std::string filelist_dest_fn = "urbackup/data/filelist.ub";
	if (index_group != c_group_default)
	{
		filelist_dest_fn = "urbackup/data/filelist_" + convert(index_group) + ".ub";
	}

	IFile* last_filelist_f = NULL;
	ScopedFreeObjRef<IFile*> last_filelist_f_scope(last_filelist_f);
	if (index_follow_last)
	{
		last_filelist_f = Server->openFile(filelist_dest_fn, MODE_READ);

		if (last_filelist_f == NULL)
		{
			index_follow_last = false;
		}

		last_filelist.reset(new SLastFileList);
		last_filelist->f = last_filelist_f;
	}

	std::string filelist_fn = "urbackup/data/filelist_new_" + convert(index_group) + ".ub";

	{
		std::fstream outfile(filelist_fn.c_str(), std::ios::out|std::ios::binary);

#ifdef _WIN32
		if (index_group == 0)
		{
			if (backup_with_vss_components)
			{
				index_flags = EBackupDirFlag_FollowSymlinks | EBackupDirFlag_SymlinksOptional | EBackupDirFlag_ShareHashes;
				index_follow_last = false;
				VSS_ID ssetid;
				if (!start_shadowcopy_components(ssetid, &has_active_transaction))
				{
					index_error = true;
					VSSLog("Indexing Windows components failed", LL_ERROR);
					return;
				}
				else
				{
					addScRefs(ssetid, past_refs);

					SCDirs* empty = NULL;
					postSnapshotProcessing(empty, full_backup);

					for (size_t k = 0; k < sc_refs.size(); ++k)
					{
						if (sc_refs[k]->ssetid == ssetid)
						{
							if (sc_refs[k]->cbt)
							{
								sc_refs[k]->cbt = finishCbt(sc_refs[k]->target, -1, sc_refs[k]->volpath, false);
							}
							postSnapshotProcessing(sc_refs[k], full_backup);
						}
					}

					if (!vss_all_components.empty())
					{
						bool orig_with_sequence = with_sequence;
						with_sequence = false;
						indexVssComponents(ssetid, !full_backup, past_refs, outfile);
						with_sequence = orig_with_sequence;
					}
				}
			}
		}

		removeUnconfirmedVssDirs();
#endif

		for(size_t i=0;i<backup_dirs.size();++i)
		{
			if(backup_dirs[i].group!=index_group)
			{
				continue;
			}
			
			if(backup_dirs[i].symlinked &&
			    !backup_dirs[i].symlinked_confirmed)
			{
				continue;
			}

			index_server_default = backup_dirs[i].server_default;

			SCDirs *scd=getSCDir(backup_dirs[i].tname, index_clientsubname, false);
			if(!scd->running)
			{
				scd->dir=backup_dirs[i].tname;
				scd->starttime=Server->getTimeSeconds();
				scd->target=getShareDir(backup_dirs[i].tname);
				scd->orig_target=scd->target;
			}
			scd->fileserv=true;

			std::string mod_path=backup_dirs[i].path;

#ifdef _WIN32
			if(mod_path.size()==2) //e.g. C:
			{
				mod_path+=os_file_sep();
			}
#endif
			int filetype = os_get_file_type(os_file_prefix(mod_path));

			bool shadowcopy_optional = (backup_dirs[i].flags & EBackupDirFlag_Optional)
				|| (backup_dirs[i].symlinked && (backup_dirs[i].flags & EBackupDirFlag_SymlinksOptional) );

			bool onlyref=false;
			bool stale_shadowcopy=false;
			bool shadowcopy_ok=false;
			bool shadowcopy_not_configured = false;

			if(filetype!=0 || !shadowcopy_optional)
			{
				VSSLog("Creating shadowcopy of \""+scd->dir+"\" in indexDirs()", LL_DEBUG);	
				shadowcopy_ok=start_shadowcopy(scd, &onlyref, true, simultaneous_other, past_refs,
					false, &stale_shadowcopy, &shadowcopy_not_configured, &has_active_transaction);
				VSSLog("done.", LL_DEBUG);
			}
			else if(shadowcopy_optional)
			{
				onlyref = true;
				std::string err_msg;
				int64 errcode = os_last_error(err_msg);

				VSSLog("Cannot access \""+mod_path+"\". Not creating snapshot. Errorcode: "+convert(errcode)+" - "+trim(err_msg), LL_DEBUG);
			}

			if(stale_shadowcopy)
			{
				has_stale_shadowcopy=true;
			}

			if(!shadowcopy_ok)
			{
				if(!shadowcopy_optional && !shadowcopy_not_configured)
				{
					VSSLog("Creating snapshot of \""+scd->dir+"\" failed.", LL_ERROR);
				}
				else
				{
					VSSLog("Backing up \"" + scd->dir + "\" without snapshot.", LL_INFO);
				}

				if (backup_dirs[i].flags & EBackupDirFlag_RequireSnapshot)
				{
					index_error = true;
				}
				else
				{
					shareDir(starttoken, backup_dirs[i].tname, removeDirectorySeparatorAtEnd(backup_dirs[i].path));
				}

				if (!disableCbt(backup_dirs[i].path))
				{
					VSSLog("Error disabling change block tracking", LL_ERROR);
					index_error = true;
				}
			}
			else
			{
				mod_path=scd->target;
				scd->running=true;
			}

			if (!index_error && !stop_index)
			{
				mod_path = removeDirectorySeparatorAtEnd(mod_path);
				backup_dirs[i].path = removeDirectorySeparatorAtEnd(backup_dirs[i].path);

#ifdef _WIN32
				if (mod_path.size() == 2) //e.g. C:
				{
					mod_path += os_file_sep();
				}
#endif
				std::string extra;

				std::string volume = backup_dirs[i].path;
				normalizeVolume(volume);

#ifdef _WIN32
				if (!shadowcopy_ok || !onlyref)
				{
					if (scd->ref != NULL)
					{
						addScRefs(scd->ref->ssetid, past_refs);
					}
					
					postSnapshotProcessing(scd, full_backup);
				}

				if (scd->ref != NULL
					&& !scd->ref->cbt)
				{
					if (!disableCbt(backup_dirs[i].path))
					{
						VSSLog("Error disabling change block tracking of \"" + backup_dirs[i].path + "\"...", LL_ERROR);
						index_error = true;
					}
				}

#else
				if (!onlyref)
				{
					past_refs.push_back(scd->ref);
				}
#endif

				for (size_t k = 0; k < changed_dirs.size(); ++k)
				{
					VSSLog("Changed dir: " + changed_dirs[k], LL_DEBUG);
				}

				if (!index_error)
				{
					VSSLog("Indexing \"" + backup_dirs[i].tname + "\"...", LL_DEBUG);
				}
				index_c_db = 0;
				index_c_fs = 0;
				index_c_db_update = 0;
				//db->BeginWriteTransaction();
				last_transaction_start = Server->getTimeMS();
				index_root_path = mod_path;
				index_keep_files = (backup_dirs[i].flags & EBackupDirFlag_KeepFiles) > 0
					&& !backup_dirs[i].reset_keep;

				std::string vssvolume = mod_path;
				normalizeVolume(vssvolume);

#ifndef _WIN32
				if (index_root_path.empty())
				{
					index_root_path = os_file_sep();
				}
#endif
				if (!index_error)
				{
					openCbtHdatFile(scd->ref, backup_dirs[i].tname, volume);

					initialCheck(strlower(volume), vssvolume, backup_dirs[i].path, mod_path, backup_dirs[i].tname, outfile, true,
						backup_dirs[i].flags, !full_backup, backup_dirs[i].symlinked, 0, true, true,
						index_exclude_dirs, index_include_dirs);
				}

				commitModifyFilesBuffer();
				commitAddFilesBuffer();
				commitModifyHardLinks();
				commitPhashQueue();
			}

			if(stop_index || index_error)
			{
				for(size_t k=0;k<backup_dirs.size();++k)
				{
					SCDirs *scd=getSCDir(backup_dirs[k].tname, index_clientsubname, false);
					release_shadowcopy(scd);
				}
				
				outfile.close();
				removeFile((filelist_fn));

				if(stop_index)
				{
					VSSLog("Indexing files failed, because of error", LL_ERROR);
				}

				return;
			}

			if(!backup_dirs[i].symlinked)
			{
				VSSLog("Indexing of \""+backup_dirs[i].tname+"\" done. "+convert(index_c_fs)+" filesystem lookups "+convert(index_c_db)+" db lookups and "+convert(index_c_db_update)+" db updates" , LL_INFO);
			}

			//Remove unreferenced symlinks now
			removeUnconfirmedSymlinkDirs(i+1);
		}

		if (outfile.is_open())
		{
			addBackupScripts(outfile);
		}

		std::streampos pos=outfile.tellp();
		outfile.seekg(0, std::ios::end);
		if(pos!=outfile.tellg())
		{
			outfile.close();
			bool b=os_file_truncate((filelist_fn), pos);
			if(!b)
			{
				VSSLog("Error changing filelist size", LL_ERROR);
			}

			outfile.open(filelist_fn.c_str(), std::ios::in|std::ios::out|std::ios::binary);
			if(outfile.is_open())
			{
				outfile.seekp(0, std::ios::end);
			}
			else
			{
				VSSLog("Error reopening filelist", LL_ERROR);
			}
		}
	}

	commitModifyFilesBuffer();
	commitAddFilesBuffer();
	commitModifyHardLinks();

	if (phash_queue != NULL)
	{
		CWData data;
		data.addChar(ID_PHASH_FINISH);
		addToPhashQueue(data);
	}

	commitPhashQueue();

	index_hdat_file.reset();

#ifdef _WIN32
	if(!has_stale_shadowcopy
		&& !has_active_transaction)
	{
		if(!index_error)
		{
			VSSLog("Deleting backup of changed dirs...", LL_DEBUG);
			cd->deleteSavedChangedDirs();
			cd->deleteSavedDelDirs();

			if(index_group==c_group_default)
			{
				DirectoryWatcherThread::update_last_backup_time();
				DirectoryWatcherThread::commit_last_backup_time();

				cd->updateMiscValue("last_filebackup_filetime_lower", convert(last_filebackup_filetime_new));
			}
		}
		else
		{
			VSSLog("Did not delete backup of changed dirs because there was an error while indexing which might not occur the next time.", LL_INFO);
		}
	}
	else
	{
		if (has_stale_shadowcopy)
		{
			VSSLog("Did not delete backup of changed dirs because a stale shadowcopy was used.", LL_INFO);
		}
		if (has_active_transaction)
		{
			VSSLog("Did not delete backup of changed dirs because at least one volume had an active NTFS transaction.", LL_INFO);
		}
	}

	DirectoryWatcherThread::unfreeze();
	open_files.clear();
	changed_dirs.clear();
	
#endif

	if (last_filelist_f!=NULL)
	{
		Server->destroy(last_filelist_f);
		last_filelist_f = NULL;
		last_filelist.reset();
	}

	{
		IScopedLock lock(filelist_mutex);
		if(index_group==c_group_default)
		{
			if (FileExists(filelist_dest_fn)
				&& !removeFile(filelist_dest_fn))
			{
				VSSLog("Error deleting file "+ filelist_dest_fn+". "+os_last_error_str(), LL_ERROR);
			}
			if (!moveFile(filelist_fn, filelist_dest_fn))
			{
				VSSLog("Error renaming "+ filelist_fn+" to "+ filelist_dest_fn+". " + os_last_error_str(), LL_ERROR);
				index_error = true;
			}
		}
		else
		{
			if (FileExists(filelist_dest_fn)
				&& !removeFile(filelist_dest_fn))
			{
				VSSLog("Error deleting file "+ filelist_dest_fn+". " + os_last_error_str(), LL_ERROR);
			}
			if (!moveFile(filelist_fn, filelist_dest_fn))
			{
				VSSLog("Error renaming "+ filelist_fn+" to "+ filelist_dest_fn+". " + os_last_error_str(), LL_ERROR);
				index_error = true;
			}
		}
	}

	for (size_t i = 0; i < backup_dirs.size(); ++i)
	{
		if (backup_dirs[i].group == index_group
			&& backup_dirs[i].reset_keep)
		{
			cd->setResetKeep(0, backup_dirs[i].id);
		}
	}

	share_dirs();

	for (size_t i = 0; i < backup_dirs.size(); ++i)
	{
		if (backup_dirs[i].group == index_group
			&& (!backup_dirs[i].symlinked || backup_dirs[i].symlinked_confirmed) )
		{
			log_read_errors(starttoken + "|" + backup_dirs[i].tname, backup_dirs[i].path);
			log_read_errors(backup_dirs[i].tname, backup_dirs[i].path);
		}
	}

	changed_dirs.clear();
}

void IndexThread::resetFileEntries(void)
{
	db->Write("DELETE FROM files WHERE tgroup=0 OR tgroup="+convert(index_group+1));
	cd->deleteSavedChangedDirs();
	cd->resetAllHardlinks();
#ifdef _WIN32
	DirectoryWatcherThread::reset_mdirs(std::string());
#endif
}

bool IndexThread::skipFile(const std::string& filepath, const std::string& namedpath,
	const std::vector<std::string>& exclude_dirs,
	const std::vector<SIndexInclude>& include_dirs)
{
	if( isExcluded(exclude_dirs, filepath) || isExcluded(exclude_dirs, namedpath) )
	{
		return true;
	}
	if( !isIncluded(include_dirs, filepath, NULL)
		&& !isIncluded(include_dirs, namedpath, NULL) )
	{
		return true;
	}

	return false;
}

bool IndexThread::initialCheck(const std::string& volume, const std::string& vssvolume, std::string orig_dir, std::string dir, std::string named_path, std::fstream &outfile,
	bool first, int flags, bool use_db, bool symlinked, size_t depth, bool dir_recurse, bool include_exclude_dirs, const std::vector<std::string>& exclude_dirs,
	const std::vector<SIndexInclude>& include_dirs, std::string orig_path)
{
	bool has_include=false;
	index_flags=flags;

	if( IdleCheckerThread::getIdle()==false )
	{
		Server->wait(nonidlesleeptime);
	}
	if(IdleCheckerThread::getPause())
	{
		Server->wait(5000);
	}

	if(stop_index)
	{
		return false;
	}

	std::string fn_filter;
	bool close_dir=false;
	std::string extra;
	
	if(first)
	{
		std::string curr_dir = os_file_prefix(dir);
		int filetype = os_get_file_type(curr_dir);

		if(filetype==0)
		{
			curr_dir = os_file_prefix(add_trailing_slash(dir));
			filetype = os_get_file_type(curr_dir);
		}

		if(!(filetype & EFileType_File) && !(filetype & EFileType_Directory))
		{
			if(!(flags & EBackupDirFlag_Optional) && (!symlinked || !(flags & EBackupDirFlag_SymlinksOptional) ) )
			{
				std::string err_msg;
				int64 errcode = os_last_error(err_msg);

				VSSLog("Cannot access path to backup: \""+dir+"\" Errorcode: "+convert(errcode)+" - "+trim(err_msg), LL_ERROR);
				index_error=true;

#ifdef _WIN32
				if (!getbetween("%", "%", orig_dir).empty())
				{
					VSSLog("Hint: The path to backup contains Windows environment variables. This is not supported. "
						"UrBackup Clients runs as user independent system service. See https://www.urbackup.org/faq.html#include_files for what you probably want to do.", LL_WARNING);
				}
#endif
				if (!os_directory_exists(orig_dir))
				{
					if (orig_dir.find("*") != std::string::npos || orig_dir.find("?") != std::string::npos)
					{
						VSSLog("Hint: The directory to backup contains wild cards. This is not supported. "
							"Please use the include and exclude settings to accomplish what you want", LL_WARNING);
					}

					VSSLog("Hint: Directory to backup (\"" + orig_dir + "\") does not exist. It may have been deleted or renamed. "
						"Set the \"optional\" directory flag if you do not want backups to fail if directories are missing.", LL_WARNING);
				}
			}

			return false;
		}

		if(with_orig_path)
		{
			extra += "&orig_path=" + EscapeParamString(orig_path.empty() ? (orig_dir.empty() ? os_file_sep() : orig_dir) : orig_path)
				+ "&orig_sep=" + EscapeParamString(os_file_sep());
		}

#ifdef _WIN32
		int64 sequence_id;
		int64 sequence_next = getUsnNum(dir, sequence_id);
		if(sequence_next!=-1 && with_sequence)
		{
			extra += "&sequence_next="+convert(sequence_next) +
				"&sequence_id="+convert(sequence_id);
		}
#endif

		if(filetype & EFileType_File)
		{
			fn_filter = ExtractFileName(dir, os_file_sep());
			orig_dir = ExtractFilePath(orig_dir, os_file_sep());
			dir = ExtractFilePath(dir, os_file_sep());
		}
		else if(filetype & EFileType_Directory)
		{
			close_dir=true;

			SFile metadata = getFileMetadataWin(curr_dir, true);

			if(metadata.usn==0)
			{
				metadata.usn = metadata.last_modified;
			}
			
			addFromLastUpto(named_path, true, depth, false, outfile);
			writeDir(outfile, named_path, with_orig_path, metadata.usn, extra);
			extra.clear();

			++depth;
		}
	}

	int64 target_generation;
	std::vector<SFileAndHash> files=getFilesProxy(orig_dir, dir, named_path, !first && use_db, fn_filter, use_db,
		exclude_dirs, include_dirs, target_generation);

	if(index_error)
	{
		return false;
	}

	bool finish_phash_path = false;
	
	for(size_t i=0;i<files.size();++i)
	{
		if( !files[i].isdir )
		{
			if( (files[i].issym && !with_proper_symlinks && !(flags & EBackupDirFlag_FollowSymlinks)) 
				|| (files[i].isspecialf && !with_proper_symlinks) )
			{
				continue;
			}

			if( skipFile(orig_dir+os_file_sep()+files[i].name, named_path+os_file_sep()+files[i].name,
				exclude_dirs, include_dirs) )
			{
				continue;
			}

			if (!use_db
				&& files[i].nlinks > 1)
			{
				enumerateHardLinks(volume, vssvolume, dir + os_file_sep() + files[i].name);
			}

			has_include=true;

			std::string listname = files[i].name;

			if(first && !fn_filter.empty() && i==0)
			{
				listname = named_path;
			}
			
			addFromLastUpto(listname, false, depth, false, outfile);

			outfile << "f\"" << escapeListName((listname)) << "\" " << files[i].size << " " << static_cast<int64>(files[i].change_indicator);
		
			if(calculate_filehashes_on_client
				&& !files[i].hash.empty() )
			{
				if(sha_version==256)
				{
					extra+="&sha256="+base64_encode_dash(files[i].hash);
				}
				else if (sha_version == 528)
				{
					extra += "&thash=" + base64_encode_dash(files[i].hash);
				}
				else
				{
					extra += "&sha512=" + base64_encode_dash(files[i].hash);
				}
			}
			else if (calculate_filehashes_on_client
				&& phash_queue != NULL
				&& !files[i].isspecialf)
			{
				if (!finish_phash_path)
				{
					finish_phash_path = true;

					CWData wdata;
					wdata.addChar(ID_SET_CURR_DIRS);
					wdata.addString2(orig_dir);
					wdata.addInt(index_group);
					wdata.addString2(dir);
					addToPhashQueue(wdata);
				}

				CWData wdata;
				wdata.addChar(ID_HASH_FILE);
				wdata.addVarInt(file_id);
				wdata.addString2(files[i].name);
				addToPhashQueue(wdata);
			}

			++file_id;
				
			if(end_to_end_file_backup_verification && !files[i].isspecialf)
			{
				extra+="&sha256_verify=" + getSHA256(dir+os_file_sep()+files[i].name);
			}

			if(files[i].issym && with_proper_symlinks)
			{
				extra+="&sym_target="+EscapeParamString((files[i].output_symlink_target));
			}
			
			if(files[i].isspecialf && with_proper_symlinks)
			{
				extra+="&special=1";
			}

			if(!extra.empty())
			{
				extra[0]='#';
				outfile << extra;
				extra.clear();
			}

			outfile << "\n";
		}
	}

	if (finish_phash_path)
	{
		CWData wdata;
		wdata.addChar(ID_FINISH_CURR_DIR);
		wdata.addVarInt(target_generation);
		addToPhashQueue(wdata);
	}

	for(size_t i=0;dir_recurse && i<files.size();++i)
	{
		if( files[i].isdir )
		{
			if( (files[i].issym && !with_proper_symlinks && !(flags & EBackupDirFlag_FollowSymlinks) ) 
				|| (files[i].isspecialf && !with_proper_symlinks) )
			{
				continue;
			}

			bool curr_included = false;
			bool adding_worthless1, adding_worthless2;

			if (include_exclude_dirs)
			{
				if (isExcluded(exclude_dirs, orig_dir + os_file_sep() + files[i].name)
					|| isExcluded(exclude_dirs, named_path + os_file_sep() + files[i].name))
				{
					continue;
				}
				
				if (isIncluded(include_dirs, orig_dir + os_file_sep() + files[i].name, &adding_worthless1)
					|| isIncluded(include_dirs, named_path + os_file_sep() + files[i].name, &adding_worthless2))
				{
					has_include = true;
					curr_included = true;
				}
			}
			else
			{
				curr_included = true;
				adding_worthless1 = false;
				adding_worthless2 = false;
			}

			if( curr_included ||  !adding_worthless1 || !adding_worthless2 )
			{
				std::streampos pos=outfile.tellp();

				SLastFileList backup;
				if (index_follow_last)
				{
					backup = *last_filelist.get();
				}

				if(files[i].issym && with_proper_symlinks)
				{
					extra+="&sym_target="+EscapeParamString((files[i].output_symlink_target));
				}

				if(files[i].isspecialf && with_proper_symlinks)
				{
					extra+="&special=1";
				}

				std::string listname = files[i].name;

				if(first && !fn_filter.empty() && i==0)
				{
					listname = named_path;
				}

				addFromLastUpto(listname, true, depth, false, outfile);
				writeDir(outfile, listname, with_orig_path, files[i].change_indicator, extra);
				extra.clear();

				bool b=true;

				if(!files[i].issym || !with_proper_symlinks)
				{
					b = initialCheck(volume, vssvolume, orig_dir+os_file_sep()+files[i].name, dir+os_file_sep()+files[i].name,
							named_path+os_file_sep()+files[i].name, outfile, false, flags, use_db, false, depth+1,
						dir_recurse, include_exclude_dirs, exclude_dirs, include_dirs);
				}

				addFromLastLiftDepth(depth, outfile);

				if(!with_proper_symlinks)
				{
					outfile << "d\"..\"\n";
				}
				else
				{
					outfile << "u\n";
				}

				++file_id;
				

				if(!b)
				{
					if(!curr_included)
					{
						if (index_follow_last)
						{
							last_filelist->reset_to(backup);
						}

						outfile.seekp(pos);
					}
				}
				else
				{
					has_include=true;
				}

				if(index_error)
				{
					return false;
				}
			}
		}
	}

	if(close_dir)
	{
		addFromLastLiftDepth(depth - 1, outfile);
		if(!with_proper_symlinks)
		{
			outfile << "d\"..\"\n";
		}
		else
		{
			outfile << "u\n";
		}

		++file_id;
	}

	return has_include;
}

bool IndexThread::readBackupDirs(void)
{
	backup_dirs=cd->getBackupDirs();

	bool has_backup_dir = false;
	for(size_t i=0;i<backup_dirs.size();++i)
	{
		backup_dirs[i].path=os_get_final_path(backup_dirs[i].path);
		Server->Log("Final path: "+backup_dirs[i].path, LL_INFO);

		if(index_group!=-1 && backup_dirs[i].group == index_group)
		{
			has_backup_dir=true;
		}

		if(filesrv!=NULL)
			shareDir("", backup_dirs[i].tname, backup_dirs[i].path);
	}
	return has_backup_dir;
}

bool IndexThread::readBackupScripts(bool full_backup)
{
	scripts.clear();

	if(!with_scripts || index_group!=c_group_default)
	{
		return false;
	}
	
	std::string script_path = Server->getServerParameter("script_path");
	std::string script_cmd;

	if(script_path.empty())
	{
#ifdef _WIN32
		script_path = Server->getServerWorkingDir() + os_file_sep() + "backup_scripts";
#else
		script_path = "/etc/urbackup/scripts";
#endif
	}

	std::vector<std::string> script_paths;
#ifndef _WIN32
	TokenizeMail(script_path, script_paths, ":");
#else
	script_paths.push_back(script_path);
#endif

	std::string first_script_path;
	for (size_t j = 0; j < script_paths.size(); ++j)
	{
		std::string curr_script_path = script_paths[j];

		if (j == 0)
		{
			first_script_path = curr_script_path;
		}

#ifdef _WIN32
		script_cmd = curr_script_path + os_file_sep() + "list.bat";
#else
		script_cmd = curr_script_path + os_file_sep() + "list";
#endif

		if (!FileExists(script_cmd))
		{
			Server->Log("Script list at \"" + script_cmd + "\" does not exist. Skipping.", LL_INFO);
			continue;
		}

		std::string output = execute_script(script_cmd, full_backup?"0":"1");

		std::vector<std::string> lines;
		Tokenize(output, lines, "\n");

		IScopedLock lock(filesrv_mutex);

		if (!lines.empty())
		{
			for (size_t i = 0; i < lines.size(); ++i)
			{
				std::string line = trim(lines[i]);
				if (!line.empty())
				{
					if (line[0] == '#')
					{
						continue;
					}

					SBackupScript new_script;

					str_map params;
					ParseParamStrHttp(line, &params);

					str_map::iterator it = params.find("scriptname");
					if (it == params.end())
					{
						continue;
					}

					new_script.scriptname = it->second;

					it = params.find("outputname");
					if (it != params.end())
					{
						new_script.outputname = it->second;
					}
					else
					{
						new_script.outputname = new_script.scriptname;
					}

					it = params.find("size");
					if (it != params.end())
					{
						new_script.size = watoi64(it->second);
					}
					else
					{
						new_script.size = -1;
					}

					bool tar_file = false;
					it = params.find("tar");
					if (it != params.end()
						&& it->second == "1")
					{
						new_script.size = 0;
						tar_file = true;
					}

					it = params.find("orig_path");
					if (it != params.end())
					{
						new_script.orig_path = it->second;
					}

					scripts.push_back(new_script);

					if (filesrv != NULL)
					{
						filesrv->addScriptOutputFilenameMapping(new_script.outputname,
							new_script.scriptname, tar_file);

						if (j > 0)
						{
							filesrv->registerFnRedirect(first_script_path + os_file_sep() + new_script.outputname,
								curr_script_path + os_file_sep() + new_script.outputname);
						}
					}
				}
			}
		}
	}

	if (filesrv != NULL && !scripts.empty() && !first_script_path.empty())
	{
		filesrv->shareDir("urbackup_backup_scripts", first_script_path, std::string(), true);
	}

	std::sort(scripts.begin(), scripts.end());
	
	return !scripts.empty();	
}

bool IndexThread::addMissingHashes(std::vector<SFileAndHash>* dbfiles, std::vector<SFileAndHash>* fsfiles, const std::string &orig_path,
	const std::string& filepath, const std::string& namedpath, const std::vector<std::string>& exclude_dirs,
	const std::vector<SIndexInclude>& include_dirs, bool calc_hashes)
{
	bool calculated_hash=false;

	if(fsfiles!=NULL)
	{
		for(size_t i=0;i<fsfiles->size();++i)
		{
			SFileAndHash& fsfile = fsfiles->at(i);
			if( fsfile.isdir || fsfile.isspecialf)
				continue;

			if(!fsfile.hash.empty())
				continue;

			if (skipFile(orig_path + os_file_sep() + fsfile.name, namedpath + os_file_sep() + fsfile.name, exclude_dirs, include_dirs))
			{
				continue;
			}

			bool needs_hashing=true;

			if(dbfiles!=NULL)
			{
				std::vector<SFileAndHash>::iterator it = std::lower_bound(dbfiles->begin(), dbfiles->end(), fsfile);

				if( it!=dbfiles->end()
					&& it->name==fsfile.name
					&& it->isdir==false
					&& it->change_indicator==fsfile.change_indicator
					&& it->size==fsfile.size
					&& !it->hash.empty() )
				{
					fsfile.hash=it->hash;
					needs_hashing=false;
				}
			}

			if(needs_hashing
				&& calc_hashes)
			{
				fsfile.hash=getShaBinary(filepath+os_file_sep()+fsfile.name);
				calculated_hash=true;
			}
		}
	}
	else if(dbfiles!=NULL
		&& calc_hashes)
	{
		for(size_t i=0;i<dbfiles->size();++i)
		{
			SFileAndHash& dbfile = dbfiles->at(i);
			if( dbfile.isdir || dbfile.isspecialf)
				continue;

			if(!dbfile.hash.empty())
				continue;

			if (skipFile(orig_path + os_file_sep() + dbfile.name, namedpath + os_file_sep() + dbfile.name, exclude_dirs, include_dirs))
			{
				continue;
			}

			dbfile.hash=getShaBinary(filepath+os_file_sep()+dbfile.name);
			calculated_hash=true;
		}
	}

	return calculated_hash;
}

std::vector<SFileAndHash> IndexThread::getFilesProxy(const std::string &orig_path, std::string path, const std::string& named_path,
	bool use_db, const std::string& fn_filter, bool use_db_hashes, const std::vector<std::string>& exclude_dirs,
	const std::vector<SIndexInclude>& include_dirs, int64& target_generation)
{
	target_generation = 0;
#ifndef _WIN32
	if(path.empty())
	{
		path = os_file_sep();
	}
	std::string path_lower=orig_path + os_file_sep();
#else
	std::string path_lower=strlower(orig_path+os_file_sep());
#endif

	std::vector<std::string>::iterator it_dir=changed_dirs.end();
#ifdef _WIN32

	bool dir_changed=std::binary_search(changed_dirs.begin(), changed_dirs.end(), path_lower);
	
	if(path_lower==strlower(Server->getServerWorkingDir())+os_file_sep()+"urbackup"+os_file_sep())
	{
		use_db=false;
	}
#else
	use_db=false;
	bool dir_changed=true;
#endif
	std::vector<SFileAndHash> fs_files;
	if (!use_db || dir_changed)
	{
		++index_c_fs;

		std::string tpath = os_file_prefix(path);

		bool has_error;
		std::vector<SFile> os_files = getFilesWin(tpath, &has_error, true, true, (index_flags & EBackupDirFlag_OneFilesystem) > 0);
		filterEncryptedFiles(path, orig_path, os_files);
		fs_files = convertToFileAndHash(orig_path, named_path, exclude_dirs, include_dirs, os_files, fn_filter);

		if (has_error)
		{
#ifdef _WIN32
			int err = GetLastError();
#else
			int err = errno;
#endif

			bool root_exists = os_directory_exists(os_file_prefix(index_root_path)) ||
				os_directory_exists(os_file_prefix(add_trailing_slash(index_root_path)));

			if (root_exists)
			{
#ifdef _WIN32
				VSSLog("Error while getting files in folder \"" + path + "\". SYSTEM may not have permissions to access this folder. Windows errorcode: " + convert(err), LL_ERROR);
#else
				VSSLog("Error while getting files in folder \"" + path + "\". User may not have permissions to access this folder. Errno is " + convert(err), LL_ERROR);
				index_error = true;
#endif
			}
			else
			{
#ifdef _WIN32
				VSSLog("Error while getting files in folder \"" + path + "\". Windows errorcode: " + convert(err) + ". Access to root directory is gone too. Shadow copy was probably deleted while indexing.", LL_ERROR);
#else
				VSSLog("Error while getting files in folder \"" + path + "\". Errorno is " + convert(err) + ". Access to root directory is gone too. Snapshot was probably deleted while indexing.", LL_ERROR);
#endif
				index_error = true;
			}
		}

		std::vector<SFileAndHash> db_files;
		bool has_files = false;

		if (use_db_hashes)
		{
#ifndef _WIN32
			if (calculate_filehashes_on_client)
			{
#endif
				has_files = cd->getFiles(path_lower, get_db_tgroup(), db_files, target_generation);
#ifndef _WIN32
			}
#endif
		}

#ifdef _WIN32
		if(dir_changed)
		{
			VSSLog("Indexing changed dir: " + path, LL_DEBUG);

			for(size_t i=0;i<fs_files.size();++i)
			{
				if(!fs_files[i].isdir)
				{
					if( std::binary_search(open_files.begin(), open_files.end(), path_lower+strlower(fs_files[i].name) ) )
					{
						VSSLog("File is open: " + fs_files[i].name, LL_DEBUG);

						if (fs_files[i].change_indicator == 0)
							fs_files[i].change_indicator += Server->getRandomNumber();

						fs_files[i].change_indicator *= (std::max)((unsigned int)2, Server->getRandomNumber());
						fs_files[i].change_indicator *= (std::max)((unsigned int)2, Server->getRandomNumber());
					}
				}
			}
		}
#endif


		if(calculate_filehashes_on_client
			&& (phash_queue==NULL || has_files) )
		{
			addMissingHashes(has_files ? &db_files : NULL, &fs_files, orig_path,
				path, named_path, exclude_dirs, include_dirs, phash_queue==NULL);
		}

		if( has_files)
		{
			if(fs_files!=db_files)
			{
				++index_c_db_update;
				modifyFilesInt(path_lower, get_db_tgroup(), fs_files, target_generation);
			}
		}
		else
		{
#ifndef _WIN32
			if(calculate_filehashes_on_client)
			{
#endif
				addFilesInt(path_lower, get_db_tgroup(), fs_files);
#ifndef _WIN32
			}
#endif
		}

		return fs_files;
	}
#ifdef _WIN32
	else
	{	
		if( cd->getFiles(path_lower, get_db_tgroup(), fs_files, target_generation) )
		{
			++index_c_db;

			handleSymlinks(orig_path, named_path, exclude_dirs, include_dirs, fs_files);

			if(calculate_filehashes_on_client)
			{
				if(addMissingHashes(&fs_files, NULL, orig_path, path, named_path,
					exclude_dirs, include_dirs, phash_queue==NULL))
				{
					++index_c_db_update;
					modifyFilesInt(path_lower, get_db_tgroup(), fs_files, target_generation);
				}
			}

			return fs_files;
		}
		else
		{
			++index_c_fs;

			std::string tpath=os_file_prefix(path);

			bool has_error;
			std::vector<SFile> os_files = getFilesWin(tpath, &has_error, true, true, (index_flags & EBackupDirFlag_OneFilesystem) > 0);
			filterEncryptedFiles(path, orig_path, os_files);
			fs_files=convertToFileAndHash(orig_path, named_path, exclude_dirs, include_dirs, os_files, fn_filter);
			if(has_error)
			{
				if(os_directory_exists(index_root_path))
				{
					VSSLog("Error while getting files in folder \""+path+"\". SYSTEM may not have permissions to access this folder. Windows errorcode: "+convert((int)GetLastError()), LL_ERROR);
				}
				else
				{
					VSSLog("Error while getting files in folder \""+path+"\". Windows errorcode: "+convert((int)GetLastError())+". Access to root directory is gone too. Shadow copy was probably deleted while indexing.", LL_ERROR);
					index_error=true;
				}
			}

			if(calculate_filehashes_on_client
				&& phash_queue==NULL)
			{
				addMissingHashes(NULL, &fs_files, orig_path, path, named_path,
					exclude_dirs, include_dirs, true);
			}

			addFilesInt(path_lower, get_db_tgroup(), fs_files);
			return fs_files;
		}
	}
#else //_WIN32
	return fs_files;
#endif
}

IPipe * IndexThread::getMsgPipe(void)
{
	return msgpipe;
}

void IndexThread::stopIndex(void)
{
	stop_index=true;
}

bool IndexThread::find_existing_shadowcopy(SCDirs *dir, bool *onlyref, bool allow_restart, bool simultaneous_other, const std::string& wpath,
	const std::vector<SCRef*>& no_restart_refs, bool for_imagebackup, bool *stale_shadowcopy, bool consider_only_own_tokens,
	bool share_new)
{
	for (size_t i = sc_refs.size(); i-- > 0;)
	{
		if (i >= sc_refs.size())
		{
			continue;
		}
#ifndef _WIN32
		std::string target_lower = sc_refs[i]->target;
		std::string wpath_lower = wpath;
#else
		std::string target_lower = strlower(sc_refs[i]->target);
		std::string wpath_lower = strlower(wpath);
#endif
		if(target_lower == wpath_lower && sc_refs[i]->ok
			&& sc_refs[i]->clientsubname == index_clientsubname )
		{
			bool do_restart = std::find(no_restart_refs.begin(),
				no_restart_refs.end(), sc_refs[i])==no_restart_refs.end();

			bool only_own_tokens=true;
			for(size_t k=0;k<sc_refs[i]->starttokens.size();++k)
			{
				int64 last_token_time = ClientConnector::getLastTokenTime(sc_refs[i]->starttokens[k]);
				int64 curr_time = Server->getTimeSeconds();
				bool token_timeout=true;
				if(curr_time>=last_token_time && curr_time-last_token_time<10*60*1000)
				{
					token_timeout=false;
				}
				if( sc_refs[i]->starttokens[k]!=starttoken && !token_timeout)
				{
					only_own_tokens=false;
					break;
				}
			}

			if(consider_only_own_tokens && !only_own_tokens)
				continue;


			bool cannot_open_shadowcopy = false;

#ifdef _WIN32
			IFile *volf=Server->openFile(sc_refs[i]->volpath, MODE_READ_DEVICE);
			if(volf==NULL)
			{
				cannot_open_shadowcopy=true;
			}
			else
			{
				Server->destroy(volf);
			}

#else
			cannot_open_shadowcopy = !os_directory_exists(sc_refs[i]->volpath);
#endif

			if(cannot_open_shadowcopy)
			{
				Server->Log("Could not open snapshot at \"" + sc_refs[i]->volpath + "\"", LL_WARNING);

				if(!do_restart)
				{
					VSSLog("Cannot open shadowcopy. Creating new or choosing other.", LL_WARNING);
					continue;
				}
				else
				{
					VSSLog("Removing reference because shadowcopy could not be openend", LL_WARNING);
				}
			}
			
			if( do_restart && allow_restart && (Server->getTimeSeconds()-sc_refs[i]->starttime>shadowcopy_startnew_timeout/1000
				|| only_own_tokens 
				|| cannot_open_shadowcopy ) )
			{
				if ( (sc_refs[i]->for_imagebackup == for_imagebackup)
					|| !simultaneous_other )
				{
					if (only_own_tokens)
					{
						VSSLog("Restarting shadow copy of " + sc_refs[i]->target + " because it was started by this server", LL_WARNING);
					}
					else if (!cannot_open_shadowcopy)
					{
						VSSLog("Restarting/not using already existing shadow copy of " + sc_refs[i]->target + " because it is too old", LL_INFO);
					}

					SCRef *curr = sc_refs[i];
					std::map<std::string, SCDirs*>& scdirs_server = scdirs[SCDirServerKey(starttoken, index_clientsubname, sc_refs[i]->for_imagebackup)];

					VSS_ID ssetid = curr->ssetid;

					std::vector<std::string> paths;
					for (std::map<std::string, SCDirs*>::iterator it = scdirs_server.begin();
						it != scdirs_server.end(); ++it)
					{
						paths.push_back(it->first);
					}

					for (size_t j = 0; j < paths.size(); ++j)
					{
						std::map<std::string, SCDirs*>::iterator it = scdirs_server.find(paths[j]);
						if (it != scdirs_server.end()
							&& it->second->ref == curr)
						{
							VSSLog("Releasing " + it->first + " orig_target=" + it->second->orig_target + " target=" + it->second->target, LL_DEBUG);
							release_shadowcopy(it->second, false, -1, dir);
						}
					}

					bool retry = true;
					while (retry)
					{
						retry = false;
						for (std::map<std::string, SCDirs*>::iterator it = scdirs_server.begin();
							it != scdirs_server.end();++it)
						{
							if (it->second->ref != NULL
								&& it->second->ref != curr
								&& it->second->ref->ssetid == ssetid)
							{
								VSSLog("Releasing group shadow copy " + it->first + " orig_target=" + it->second->orig_target + " target=" + it->second->target, LL_DEBUG);
								release_shadowcopy(it->second, false, -1, dir);
								retry = true;
								break;
							}
						}
					}
					dir->target = dir->orig_target;
					continue;
				}
				else
				{
					VSSLog("Not restarting/using existing shadow copy of " + sc_refs[i]->target + 
						" because it was not created for image backups/file backups and there is a simultaneous other backup (for_imagebackup="+convert(for_imagebackup)+")", LL_INFO);
				}
			}
			else if(!cannot_open_shadowcopy)
			{
				dir->ref=sc_refs[i];
				if(!dir->ref->dontincrement)
				{
					sc_refs[i]->starttokens.push_back(starttoken);
				}
				else
				{
					dir->ref->dontincrement=false;
				}

				VSSLog("orig_target="+dir->orig_target+" volpath="+dir->ref->volpath, LL_DEBUG);

				dir->target=dir->orig_target;
				dir->target.erase(0,wpath.size());

#ifndef _WIN32
				if(dir->target.empty() || dir->target[0]!='/')
				{
					dir->target = "/" + dir->target;
				}
				dir->target=dir->ref->volpath+ (dir->target.empty()? "" : dir->target);
#else
				dir->target=dir->ref->volpath+os_file_sep()+dir->target;
#endif
				if(dir->fileserv
					&& share_new)
				{
					shareDir(starttoken, dir->dir, dir->target);
				}

				if(for_imagebackup && dir->ref->save_id!=-1)
				{
					cd->modShadowcopyRefCount(dir->ref->save_id, 1);
				}

				if(onlyref!=NULL)
				{
					*onlyref=true;
				}

				if( stale_shadowcopy!=NULL )
				{
					if(!do_restart)
					{
						*stale_shadowcopy=false;
					}
					else if(!only_own_tokens || !allow_restart)
					{
						*stale_shadowcopy=true;
					}
				}

				VSSLog("Shadowcopy already present.", LL_DEBUG);
				return true;
			}
		}
	}

	return false;
}

bool IndexThread::start_shadowcopy(SCDirs *dir, bool *onlyref, bool allow_restart, bool simultaneous_other,
	std::vector<SCRef*> no_restart_refs, bool for_imagebackup, bool *stale_shadowcopy, bool* not_configured,
	bool* has_active_transaction)
{
	bool c_onlyref = false;
	if (onlyref != NULL)
	{
		if (*onlyref)
		{
			c_onlyref = true;
		}
		else
		{
			*onlyref = true;
		}
	}

	cleanup_saved_shadowcopies(!simultaneous_other);

#ifdef _WIN32
	WCHAR volume_path[MAX_PATH];
	BOOL ok = GetVolumePathNameW(Server->ConvertToWchar(dir->orig_target).c_str(), volume_path, MAX_PATH);
	if (!ok)
	{
		VSSLog("Cannot get volume for path \""+ dir->orig_target+"\". "+os_last_error_str(), LL_ERROR);
		return false;
	}
	std::string wpath = Server->ConvertFromWchar(volume_path);
#else
	std::string wpath = "/";

	if (get_volumes_mounted_locally())
	{
		wpath = getFolderMount(dir->orig_target);
		if (wpath.empty())
		{
			wpath = dir->orig_target;
		}
	}
	else
	{
		dir->target = wpath;
	}
#endif

	
	if(find_existing_shadowcopy(dir, onlyref, allow_restart, simultaneous_other, wpath, no_restart_refs, for_imagebackup, stale_shadowcopy, true, !c_onlyref)
		|| find_existing_shadowcopy(dir, onlyref, allow_restart, simultaneous_other, wpath, no_restart_refs, for_imagebackup, stale_shadowcopy, true, !c_onlyref) )
	{
		return true;
	}

	if (c_onlyref)
	{
		return false;
	}

	dir->ref=new SCRef;
	dir->ref->starttime=Server->getTimeSeconds();
	dir->ref->target=wpath;
	dir->ref->starttokens.push_back(starttoken);
	dir->ref->clientsubname = index_clientsubname;
	dir->ref->for_imagebackup = for_imagebackup;
	sc_refs.push_back(dir->ref);
	

#ifdef _WIN32
	bool b = start_shadowcopy_win(dir, wpath, for_imagebackup, false, onlyref, has_active_transaction);
#else
	bool b = start_shadowcopy_lin(dir, wpath, for_imagebackup, onlyref, not_configured);
#endif

	if(!b)
	{
		sc_refs.erase(sc_refs.end()-1);
		delete dir->ref;
		dir->ref=NULL;
		dir->target = dir->orig_target;
	}

	return b;
}

namespace
{
	void cleanup_shadowcopies_xp(ClientDAO *cd, SCDirs *dir)
	{
#if defined(VSS_XP) || defined(VSS_S03)
		std::vector<SShadowCopy> scs=cd->getShadowcopies();

		bool found=false;

		for(size_t i=0;i<scs.size();++i)
		{
			if( scs[i].target==dir->target || ( dir->ref!=NULL && dir->ref->backupcom==NULL )  )
			{
				found=true;
			}
		}

		if(found)
		{
			for(size_t i=0;i<scs.size();++i)
			{
				if(scs[i].target==dir->target || ( dir->ref!=NULL && dir->ref->backupcom==NULL ) )
				{
					Server->Log("Removing shadowcopy entry for path \""+scs[i].path+"\"", LL_DEBUG);
					cd->deleteShadowcopy(scs[i].id);
				}
			}
		}
#endif
	}
}


bool IndexThread::deleteShadowcopy(SCDirs *dir)
{
#ifdef _WIN32
	return deleteShadowcopyWin(dir);
#else
	std::string loglines;
	std::string scriptname;
	if(dir->fileserv)
	{
		scriptname = "remove_filesystem_snapshot";
	}
	else
	{
		scriptname = "remove_device_snapshot";
	}

	std::string scriptlocation = get_snapshot_script_location(scriptname);

	if (scriptlocation.empty())
	{
		return false;
	}

	int rc = os_popen(scriptlocation +" "+guidToString(dir->ref->ssetid)+" "+escapeDirParam(dir->ref->volpath)
		+" "+escapeDirParam(dir->dir)+" "+escapeDirParam(dir->target)+" "+escapeDirParam(dir->ref->target)
		+ (dir->ref->clientsubname.empty() ? "" : (" " + escapeDirParam(dir->ref->clientsubname)))+" 2>&1", loglines);
	if(rc!=0)
	{
		VSSLog("Error removing snapshot to "+dir->target, LL_ERROR);
		VSSLogLines(loglines, LL_ERROR);
		return false;
	}
	else
	{
		VSSLogLines(loglines, LL_INFO);
	}
	return true;
#endif
}

bool IndexThread::release_shadowcopy(SCDirs *dir, bool for_imagebackup, int save_id, SCDirs *dontdel)
{
	if(for_imagebackup)
	{
		if(dir->ref!=NULL && dir->ref->save_id!=-1)
		{
			cd->modShadowcopyRefCount(dir->ref->save_id, -1);
		}
		else if(save_id!=-1)
		{
			cd->modShadowcopyRefCount(save_id, -1);
		}
	}

	bool ok=true;

	if(dir->ref!=NULL 
#ifdef _WIN32
		&& dir->ref->backupcom!=NULL
#endif
		)
	{
		if(dir->ref->starttokens.empty() ||
			( dir->ref->starttokens.size()==1
				&& dir->ref->starttokens[0]==starttoken)
			|| Server->getTimeSeconds()-dir->ref->starttime>shadowcopy_timeout/1000)
		{

			VSSLog("Deleting shadowcopy for path \""+dir->target+"\" -2", LL_DEBUG);
			ok = deleteShadowcopy(dir);

			if(dir->ref->save_id!=-1)
			{
				cd->deleteShadowcopy(dir->ref->save_id);
			}
		}
	}

	if(dir->ref!=NULL)
	{
		for(size_t k=0;k<dir->ref->starttokens.size();++k)
		{
			if(dir->ref->starttokens[k]==starttoken)
			{
				dir->ref->starttokens.erase(dir->ref->starttokens.begin()+k);
				break;
			}
		}
	}

	cleanup_saved_shadowcopies();

	cleanup_shadowcopies_xp(cd, dir);
	

	bool r=true;
	while(r)
	{
		r=false;
		for(size_t i=0;i<sc_refs.size();++i)
		{
			if(sc_refs[i]->starttokens.empty())
			{
				VSSLog("Deleting Shadowcopy for dir \""+sc_refs[i]->target+"\"", LL_DEBUG);
				bool c=true;
				while(c)
				{
					c=false;
					for(std::map<SCDirServerKey, std::map<std::string, SCDirs*> >::iterator server_it = scdirs.begin();
						server_it!=scdirs.end();++server_it)
					{
						for(std::map<std::string, SCDirs*>::iterator it=server_it->second.begin();
							it!=server_it->second.end();++it)
						{
							if(it->second->ref==sc_refs[i])
							{
								if(it->second->fileserv)
								{
									shareDir(server_it->first.start_token, it->second->dir, it->second->orig_target);
								}
								it->second->target=it->second->orig_target;

								it->second->ref=NULL;
								if(dontdel==NULL || it->second!=dontdel )
								{
									delete it->second;
									server_it->second.erase(it);
									c=true;
									break;
								}
							}
						}
					}					
				}
				delete sc_refs[i];
				sc_refs.erase(sc_refs.begin()+i);
				r=true;
				break;
			}
		}
	}

	return ok;
}


bool IndexThread::deleteSavedShadowCopy( SShadowCopy& scs, SShadowCopyContext& context )
{
#if defined(_WIN32) && !defined(VSS_XP) && !defined(VSS_S03)
	return deleteSavedShadowCopyWin(scs, context);
#elif !defined(_WIN32)
	std::string loglines;

	std::string scriptname;
	if(scs.filesrv)
	{
		scriptname = "remove_filesystem_snapshot";
	}
	else
	{
		scriptname = "remove_device_snapshot";
	}

	std::string scriptlocation = get_snapshot_script_location(scriptname);

	if (scriptlocation.empty())
	{
		return false;
	}

	int rc = os_popen(scriptlocation + " "+guidToString(scs.ssetid)+" "+escapeDirParam(scs.path)+" "+escapeDirParam(scs.tname)
		+" "+escapeDirParam(scs.path)+" "+escapeDirParam(scs.orig_target)
		+ (scs.clientsubname.empty() ? "" : (" " + escapeDirParam(scs.clientsubname))), loglines);
	if(rc!=0)
	{
		VSSLog("Error removing snapshot to "+scs.orig_target, LL_ERROR);
		VSSLogLines(loglines, LL_ERROR);
		return false;
	}
	else
	{
		cd->deleteShadowcopy(scs.id);
		VSSLogLines(loglines, LL_INFO);
	}
	return true;
#else
	cd->deleteShadowcopy(scs.id);
	return true;
#endif
}

bool IndexThread::cleanup_saved_shadowcopies(bool start)
{
	std::vector<SShadowCopy> scs=cd->getShadowcopies();

	SShadowCopyContext context;

	bool ok=true;
	for(size_t i=0;i<scs.size();++i)
	{
		bool f2=true;
		for(size_t j=0;j<sc_refs.size();++j)
		{
			if(sc_refs[j]->save_id==scs[i].id
				|| sc_refs[j]->ssetid==scs[i].ssetid)
			{
				f2=false;
				break;
			}
		}
		if(f2 && (scs[i].refs<=0
			|| scs[i].passedtime>shadowcopy_timeout/1000
			|| (start && !scs[i].filesrv && scs[i].refs==1 && !starttoken.empty() && scs[i].starttoken==starttoken && scs[i].clientsubname==index_clientsubname ) ) )
		{
			if (!deleteSavedShadowCopy(scs[i], context))
			{
				ok = false;
			}
		}
	}

	clearContext(context);

	return ok;
}



SCDirs* IndexThread::getSCDir(const std::string& path, const std::string& clientsubname, bool for_imagebackup)
{
	std::map<std::string, SCDirs*>& scdirs_server = scdirs[SCDirServerKey(starttoken, clientsubname, for_imagebackup)];
	std::map<std::string, SCDirs*>::iterator it=scdirs_server.find(path);
	if(it!=scdirs_server.end())
	{
		return it->second;
	}
	else
	{
		SCDirs *nd=new SCDirs;
		scdirs_server.insert(std::pair<std::string, SCDirs*>(path, nd) );

		nd->running=false;

		return nd;
	}
}

IFileServ *IndexThread::getFileSrv(void)
{
	IScopedLock lock(filesrv_mutex);
	return filesrv;
}

int IndexThread::execute_hook(std::string script_name, bool incr, std::string server_token, int* index_group)
{
	if (!FileExists(script_name))
	{
		Server->Log("Script \"" + script_name + "\" does not exist", LL_DEBUG);
		return 0;
	}

	server_token = greplace("\"", "", server_token);
	server_token = greplace("\\", "", server_token);

	
#ifdef _WIN32
	//I ... waaa ... even ... :(
	std::string quoted_script_name = greplace(" ", "\" \"", script_name);
#else
	std::string quoted_script_name = "\"" + greplace("\"", "\\\"", script_name) + "\"";
#endif

	std::string output;
	int rc = os_popen(quoted_script_name + " " + (incr ? "1" : "0") + " \"" 
		+ server_token + "\" "
		+ (index_group!=NULL ? convert(*index_group) : "" )
		+" 2>&1", output);

	if (rc != 0 && !output.empty())
	{
		Server->Log("Script \"" + script_name + "\" returned with error code " + convert(rc), LL_WARNING);
		VSSLogLines(output, LL_ERROR);
	}
	else if (!output.empty())
	{
		Server->Log("Script \"" + script_name + "\" returned with success", LL_INFO);
		VSSLogLines(output, LL_INFO);
	}

	return rc;
}

int IndexThread::execute_prebackup_hook(bool incr, std::string server_token, int index_group)
{
	std::string script_name;
#ifdef _WIN32
	script_name = Server->getServerWorkingDir() + "\\prefilebackup.bat";
#else
	script_name = SYSCONFDIR "/urbackup/prefilebackup";
#endif

	return execute_hook(script_name, incr, server_token, &index_group);
}

int IndexThread::execute_postindex_hook(bool incr, std::string server_token, int index_group)
{
	std::string script_name;
#ifdef _WIN32
	script_name = Server->getServerWorkingDir() + "\\postfileindex.bat";
#else
	script_name = SYSCONFDIR "/urbackup/postfileindex";
#endif

	return execute_hook(script_name, incr, server_token, &index_group);
}

void IndexThread::execute_postbackup_hook(std::string scriptname, int group, const std::string& clientsubname)
{
#ifdef _WIN32
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(STARTUPINFO) );
	memset(&pi, 0, sizeof(PROCESS_INFORMATION) );
	si.cb=sizeof(STARTUPINFO);

	std::string clientsubname_san = greplace("\"", "", clientsubname);
	clientsubname_san = greplace("\\", "", clientsubname_san);

	std::string quoted_script_name = greplace(" ", "\" \"", Server->getServerWorkingDir() + "\\" + scriptname + ".bat");

	if (!CreateProcessW(L"C:\\Windows\\system32\\cmd.exe", (LPWSTR)(Server->ConvertToWchar("cmd.exe /C " + quoted_script_name + " " + convert(group) + " \"" + clientsubname_san + "\"")).c_str()
		, NULL, NULL, false, NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
	{
		Server->Log("Executing postfilebackup.bat failed: "+convert((int)GetLastError()), LL_INFO);
	}
	else
	{
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
#else
	pid_t pid1;
	pid1 = fork();
	if( pid1==0 )
	{
		setsid();
		pid_t pid2;
		pid2 = fork();
		if(pid2==0)
		{
			std::string fullname = std::string(SYSCONFDIR "/urbackup/") + scriptname;
			std::string group_str = convert(group);
			char* const argv[]={ const_cast<char*>(fullname.c_str()), 
				const_cast<char*>(group_str.c_str()), const_cast<char*>(clientsubname.c_str()), NULL };
			execv(const_cast<char*>(fullname.c_str()), argv);
			exit(1);
		}
		else
		{
			exit(1);
		}
	}
	else
	{
		int status;
		waitpid(pid1, &status, 0);
	}
#endif
}

std::string IndexThread::sanitizePattern(const std::string &p)
{
	std::string ep=trim(p);
	std::string nep;
	nep.reserve(ep.size()*2);
	for(size_t j=0;j<ep.size();++j)
	{
		char ch=ep[j];
		if(ch=='/')
		{
			if(os_file_sep()=="\\")
			{
				nep+="\\\\";
			}
			else
			{
				nep+=os_file_sep();
			}
		}
		else if(ch=='\\' && j+1<ep.size() && ep[j+1]=='\\')
		{
			if(os_file_sep()=="\\")
			{
				nep+="\\\\";
			}
			else
			{
				nep+=os_file_sep();
			}
			++j;
		}
		else if(ch=='\\' && ( j+1>=ep.size() || (ep[j+1]!='[' ) ) )
		{
			if(os_file_sep()=="\\")
				nep+="\\\\";
			else
				nep+=os_file_sep();
		}
		else
		{
			nep+=ch;
		}
	}
	return nep;
}

void IndexThread::readPatterns(int index_group, std::string index_clientsubname, std::vector<std::string>& exclude_dirs, std::vector<SIndexInclude>& include_dirs)
{
	std::string exclude_pattern_key = "exclude_files";
	std::string include_pattern_key = "include_files";

	if(index_group==c_group_continuous)
	{
		exclude_pattern_key="continuous_exclude_files";
		include_pattern_key="continuous_include_files";
	}

	std::string settings_fn = "urbackup/data/settings.cfg";
	if(!index_clientsubname.empty())
	{
		settings_fn = "urbackup/data/settings_"+conv_filename(index_clientsubname)+".cfg";
	}

	ISettingsReader *curr_settings=Server->createFileSettingsReader(settings_fn);
	exclude_dirs.clear();
	if(curr_settings!=NULL)
	{	
		std::string val;
		if(curr_settings->getValue(exclude_pattern_key, &val) || curr_settings->getValue(exclude_pattern_key+"_def", &val) )
		{
			exclude_dirs = parseExcludePatterns(val);
		}
		else
		{
			exclude_dirs = parseExcludePatterns(std::string());
		}

		if(curr_settings->getValue(include_pattern_key, &val) || curr_settings->getValue(include_pattern_key+"_def", &val) )
		{
			include_dirs = parseIncludePatterns(val);
		}

		Server->destroy(curr_settings);
	}
	else
	{
		exclude_dirs = parseExcludePatterns(std::string());
	}
}

void IndexThread::onReadError(const std::string& sharename, const std::string& filepath, int64 pos, const std::string& msg)
{
	SReadError read_error;
	read_error.sharename = sharename;
	read_error.filepath = filepath;
	read_error.filepos = pos;
	read_error.msg = msg;

	IScopedLock lock(read_error_mutex);

	if (std::find(read_errors.begin(), read_errors.end(), read_error)==read_errors.end())
	{
		read_errors.push_back(read_error);
	}
}

std::vector<std::string> IndexThread::parseExcludePatterns(const std::string& val)
{
	std::vector<std::string> exclude_dirs;
	if(!val.empty())
	{
		std::vector<std::string> toks;
		Tokenize(val, toks, ";");
		exclude_dirs =toks;
#ifdef _WIN32
		for(size_t i=0;i<exclude_dirs.size();++i)
		{
			strupper(&exclude_dirs[i]);
		}
#endif
		for(size_t i=0;i<exclude_dirs.size();++i)
		{
			if(exclude_dirs[i].find('\\')==std::string::npos
				&& exclude_dirs[i].find('/')==std::string::npos
				&& exclude_dirs[i].find('*')==std::string::npos )
			{
				exclude_dirs[i]="*/"+trim(exclude_dirs[i]);
			}
		}
		for(size_t i=0;i<exclude_dirs.size();++i)
		{
			exclude_dirs[i]=sanitizePattern(exclude_dirs[i]);
		}
	}	

	addFileExceptions(exclude_dirs);
	addHardExcludes(exclude_dirs);

	return exclude_dirs;
}

std::vector<SIndexInclude> IndexThread::parseIncludePatterns(const std::string& val)
{
	std::vector<std::string> toks;
	Tokenize(val, toks, ";");
	std::vector<SIndexInclude> include_dirs(toks.size());
	for(size_t i=0;i<include_dirs.size();++i)
	{
		include_dirs[i].spec = toks[i];
#ifdef _WIN32
		strupper(&include_dirs[i].spec);
#endif
	}
	for(size_t i=0;i<include_dirs.size();++i)
	{
		include_dirs[i].spec=sanitizePattern(include_dirs[i].spec);
	}

	for(size_t i=0;i<include_dirs.size();++i)
	{
		std::string ip=include_dirs[i].spec;
		if(ip.find("*")==ip.size()-1 || ip.find("*")==std::string::npos)
		{
			int depth=0;
			for(size_t j=0;j<ip.size();++j)
			{
				if(ip[j]=='/')
					++depth;
				else if(ip[j]=='\\' && j+1<ip.size() && ip[j+1]=='\\')
				{
					++j;
					++depth;
				}
			}
			include_dirs[i].depth=depth;
		}
		else
		{
			include_dirs[i].depth =-1;
		}
	}

	for(size_t i=0;i<include_dirs.size();++i)
	{
		size_t f1=include_dirs[i].spec.find_first_of(":");
		size_t f2=include_dirs[i].spec.find_first_of("[");
		size_t f3=include_dirs[i].spec.find_first_of("*");
		while(f2>0 && f2!=std::string::npos && include_dirs[i].spec[f2-1]=='\\')
			f2=include_dirs[i].spec.find_first_of("[", f2);

		size_t f=(std::min)((std::min)(f1,f2), f3);

		if(f!=std::string::npos)
		{
			if(f>0)
			{
				include_dirs[i].prefix=include_dirs[i].prefix.substr(0, f);
			}
		}
		else
		{
			include_dirs[i].prefix= include_dirs[i].prefix;
		}

		std::string nep;
		for(size_t j=0;j<include_dirs[i].prefix.size();++j)
		{
			char ch= include_dirs[i].prefix[j];
			if(ch=='/')
				nep+=os_file_sep();
			else if(ch=='\\' && j+1<include_dirs[i].prefix.size() && include_dirs[i].prefix[j+1]=='\\')
			{
				nep+=os_file_sep();
				++j;
			}
			else
			{
				nep+=ch;
			}
		}			

		include_dirs[i].prefix=nep;
	}

	return include_dirs;
}

bool IndexThread::isExcluded(const std::vector<std::string>& exclude_dirs, const std::string &path)
{
	std::string wpath=path;
#ifdef _WIN32
	strupper(&wpath);
#endif
	for(size_t i=0;i<exclude_dirs.size();++i)
	{
		if(!exclude_dirs[i].empty())
		{
			bool b=amatch(wpath.c_str(), exclude_dirs[i].c_str());
			if(b)
			{
				return true;
			}
		}
	}
	return false;
}

bool IndexThread::isIncluded(const std::vector<SIndexInclude>& include_dirs, const std::string &path, bool *adding_worthless)
{
	std::string wpath=path;
#ifdef _WIN32
	strupper(&wpath);
#endif
	int wpath_level=0;
	if(adding_worthless!=NULL)
	{
		for(size_t i=0;i<wpath.size();++i)
		{
			if(wpath[i]=='/')
				++wpath_level;
			else if(wpath[i]=='\\')
				++wpath_level;
			else if(i==wpath.size()-1)
				++wpath_level;
		}
		*adding_worthless=true;
	}
	bool has_pattern=false;
	for(size_t i=0;i<include_dirs.size();++i)
	{
		if(!include_dirs[i].spec.empty())
		{
			has_pattern=true;
			bool b=amatch(wpath.c_str(), include_dirs[i].spec.c_str());
			if(b)
			{
				return true;
			}
			if(adding_worthless!=NULL)
			{
				if( include_dirs[i].depth==-1 )
				{
					*adding_worthless=false;
				}
				else
				{
					bool has_prefix=(wpath.find(include_dirs[i].prefix)==0);
					if( has_prefix )
					{
						if( wpath_level<= include_dirs[i].depth)
						{
							*adding_worthless=false;
						}
					}
				}
			}
		}
	}
	return !has_pattern;
}

void IndexThread::start_filesrv(void)
{
	std::string name;
	if(Server->getServerParameter("restore_mode")=="true")
	{
		name="##restore##"+convert(Server->getTimeSeconds())+convert(Server->getRandomNumber()%10000);
		writestring((name), "clientname.txt");
	}
	else
	{
		ISettingsReader *curr_settings=Server->createFileSettingsReader("urbackup/data/settings.cfg");
		if(curr_settings!=NULL)
		{
			std::string val;
			if(curr_settings->getValue("computername", &val)
				|| curr_settings->getValue("computername_def", &val) )
			{
				if(!val.empty())
				{
					name=val;
				}
			}
			Server->destroy(curr_settings);
		}
	}

	unsigned int curr_tcpport = tcpport;
	unsigned int curr_udpport = udpport;
	std::string s_tcpport=Server->getServerParameter("fileserv_tcpport");
	if(!s_tcpport.empty())
		curr_tcpport=atoi(s_tcpport.c_str());
	std::string s_udpport=Server->getServerParameter("fileserv_udpport");
	if(!s_udpport.empty())
		curr_udpport=atoi(s_udpport.c_str());

	bool use_fqdn=false;
	IDatabase* db = Server->getDatabase(Server->getThreadID(), URBACKUPDB_CLIENT);
	if(db)
	{
		db_results res = db->Read("SELECT tvalue FROM misc WHERE tkey = 'use_fqdn'");
		if(!res.empty() && res[0]["tvalue"]=="1")
		{
			use_fqdn=true;
		}
	}

	if(Server->getServerParameter("internet_only_mode")=="true")
	{
		curr_tcpport=0;
		curr_udpport=0;
	}

	IFileServFactory* filesrv_fak = (IFileServFactory*)Server->getPlugin(Server->getThreadID(), filesrv_pluginid);

	filesrv=filesrv_fak->createFileServ(curr_tcpport, curr_udpport, name, use_fqdn,
		backgroundBackupsEnabled(std::string()));
	filesrv->shareDir("urbackup", Server->getServerWorkingDir()+"/urbackup/data", std::string(), false);

	ServerIdentityMgr::setFileServ(filesrv);
	ServerIdentityMgr::loadServerIdentities();

	filesrv->registerReadErrorCallback(this);
}

void IndexThread::shareDir(const std::string& token, std::string name, const std::string &path)
{
	if(!token.empty())
	{
		name = token + "|" +name;
	}

	if (name == "urbackup"
		|| name == "urbackup_backup_scripts")
	{
		Server->Log("Share named \"" + name + "\". Skipping...", LL_DEBUG);
		return;
	}

	IScopedLock lock(filesrv_mutex);
	filesrv_share_dirs[name]=path;
}

void IndexThread::removeDir(const std::string& token, std::string name)
{
	if(!token.empty())
	{
		name = token + "|" +name;
	}
	IScopedLock lock(filesrv_mutex);
	std::map<std::string, std::string>::iterator it=filesrv_share_dirs.find(name);
	if(it!=filesrv_share_dirs.end())
	{
		filesrv_share_dirs.erase(it);
	}
}

std::string IndexThread::getShareDir(const std::string &name)
{
	IScopedLock lock(filesrv_mutex);
	return filesrv_share_dirs[name];
}

void IndexThread::share_dirs()
{
	IScopedLock lock(filesrv_mutex);
	for (std::map<std::string, std::string>::iterator it = filesrv_share_dirs.begin(); it != filesrv_share_dirs.end(); ++it)
	{
		std::string dir = it->first;
		filesrv->shareDir(dir, it->second, std::string(), false);
	}
	filesrv->clearReadErrors();
}

void IndexThread::unshare_dirs()
{
	IScopedLock lock(filesrv_mutex);
	for(std::map<std::string, std::string>::iterator it=filesrv_share_dirs.begin();it!=filesrv_share_dirs.end();++it)
	{
		std::string dir=it->first;
		filesrv->removeDir(dir, std::string());
	}
}

void IndexThread::doStop(void)
{
	CWData wd;
	wd.addUChar(8);
	wd.addVoidPtr(NULL);
	msgpipe->Write(wd.getDataPtr(), wd.getDataSize());
}


size_t IndexThread::calcBufferSize( std::string &path, const std::vector<SFileAndHash> &data )
{
	size_t add_size=path.size()+sizeof(std::string)+sizeof(int)+sizeof(int64);
	for(size_t i=0;i<data.size();++i)
	{
		add_size+=data[i].name.size();
		add_size+=sizeof(SFileAndHash);
		add_size+=data[i].hash.size();
	}
	add_size+=sizeof(std::vector<SFile>);

	return add_size;
}


void IndexThread::modifyFilesInt(std::string path, int tgroup,
	const std::vector<SFileAndHash> &data, int64 target_generation)
{
	modify_file_buffer_size+=calcBufferSize(path, data);

	modify_file_buffer.push_back(SBufferItem(path, tgroup, data, target_generation));

	if(last_file_buffer_commit_time==0)
	{
		last_file_buffer_commit_time = Server->getTimeMS();
	}

	if( modify_file_buffer_size+add_file_buffer_size>max_file_buffer_size 
		|| Server->getTimeMS()-last_file_buffer_commit_time>file_buffer_commit_interval)
	{
		commitModifyFilesBuffer();
		commitAddFilesBuffer();
		commitPhashQueue();
	}
}

void IndexThread::commitModifyFilesBuffer(void)
{
	db->BeginWriteTransaction();
	for(size_t i=0;i<modify_file_buffer.size();++i)
	{
		cd->modifyFiles(modify_file_buffer[i].path, modify_file_buffer[i].tgroup,
			modify_file_buffer[i].files, modify_file_buffer[i].target_generation);
	}
	db->EndTransaction();

	modify_file_buffer.clear();
	modify_file_buffer_size=0;
	last_file_buffer_commit_time=Server->getTimeMS();
}

void IndexThread::addFilesInt( std::string path, int tgroup, const std::vector<SFileAndHash> &data )
{
	add_file_buffer_size+=calcBufferSize(path, data);

	add_file_buffer.push_back(SBufferItem(path, tgroup, data, 0));

	if(last_file_buffer_commit_time==0)
	{
		last_file_buffer_commit_time = Server->getTimeMS();
	}

	if(add_file_buffer_size+ add_file_buffer_size>max_file_buffer_size
		|| Server->getTimeMS()-last_file_buffer_commit_time>file_buffer_commit_interval)
	{
		commitAddFilesBuffer();
		commitModifyFilesBuffer();
		commitPhashQueue();
	}

}

void IndexThread::commitAddFilesBuffer()
{
	db->BeginWriteTransaction();
	for(size_t i=0;i<add_file_buffer.size();++i)
	{
		cd->addFiles(add_file_buffer[i].path, add_file_buffer[i].tgroup, add_file_buffer[i].files);
	}
	db->EndTransaction();

	add_file_buffer.clear();
	add_file_buffer_size=0;
	last_file_buffer_commit_time=Server->getTimeMS();
}


std::string IndexThread::removeDirectorySeparatorAtEnd(const std::string& path)
{
	char path_sep=os_file_sep()[0];
	if(!path.empty() && path[path.size()-1]==path_sep )
	{
		return path.substr(0, path.size()-1);
	}
	return path;
}

std::string IndexThread::addDirectorySeparatorAtEnd(const std::string& path)
{
	char path_sep=os_file_sep()[0];
	if(!path.empty() && path[path.size()-1]!=path_sep )
	{
		return path+os_file_sep();
	}
	return path;
}

std::string IndexThread::getSHA256(const std::string& fn)
{
	sha256_ctx ctx;
	sha256_init(&ctx);

	IFile * f=Server->openFile(os_file_prefix(fn), MODE_READ_SEQUENTIAL_BACKUP);

	if(f==NULL)
	{
		return std::string();
	}

	int64 fsize = f->Size();
	int64 fpos = 0;

	char buffer[32768];
	unsigned int r;
	while(fpos<fsize)
	{
		_u32 max_read = static_cast<_u32>((std::min)(static_cast<int64>(32768), fsize - fpos));
		r = f->Read(buffer, max_read);

		if (r == 0)
		{
			break;
		}

		sha256_update(&ctx, reinterpret_cast<const unsigned char*>(buffer), r);

		if(IdleCheckerThread::getPause())
		{
			Server->wait(5000);
		}

		fpos += r;
	}

	Server->destroy(f);

	unsigned char dig[32];
	sha256_final(&ctx, dig);

	return bytesToHex(dig, 32);
}

void IndexThread::VSSLog(const std::string& msg, int loglevel)
{
	Server->Log(msg, loglevel);
	if(loglevel>LL_DEBUG)
	{
		SVssLogItem item;
		item.msg = msg;
		item.loglevel = loglevel;
		item.times = Server->getTimeSeconds();
		vsslog.push_back(item);
	}
}

void IndexThread::VSSLogLines(const std::string& msg, int loglevel)
{
	std::vector<std::string> lines;
	TokenizeMail(msg, lines, "\n");

	for(size_t i=0;i<lines.size();++i)
	{
		std::string line = trim(lines[i]);
		if(!line.empty())
		{
			VSSLog(line, loglevel);
		}
	}
}


#ifdef _WIN32
namespace
{
	LONG GetStringRegKey(HKEY hKey, const std::string &strValueName, std::string &strValue, const std::string &strDefaultValue)
	{
		strValue = strDefaultValue;
		WCHAR szBuffer[8192];
		DWORD dwBufferSize = sizeof(szBuffer);
		ULONG nError;
		std::wstring rval;
		nError = RegQueryValueExW(hKey, Server->ConvertToWchar(strValueName).c_str(), 0, NULL, (LPBYTE)szBuffer, &dwBufferSize);
		if (ERROR_SUCCESS == nError)
		{
			rval.resize(dwBufferSize/sizeof(wchar_t));
			memcpy(const_cast<wchar_t*>(rval.c_str()), szBuffer, dwBufferSize);
			strValue = Server->ConvertFromWchar(rval);
		}
		return nError;
	}
}
#endif

void IndexThread::addFileExceptions(std::vector<std::string>& exclude_dirs)
{
#ifdef _WIN32
	exclude_dirs.push_back(sanitizePattern("C:\\HIBERFIL.SYS"));

	HKEY hKey;
	LONG lRes = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", 0, KEY_READ, &hKey);
	if(lRes != ERROR_SUCCESS)
		return;

	std::string tfiles;
	lRes = GetStringRegKey(hKey, "ExistingPageFiles", tfiles, "");
	if(lRes != ERROR_SUCCESS)
		return;

	std::vector<std::string> toks;
	std::string sep;
	sep.resize(1);
	sep[0]=0;
	Tokenize(tfiles, toks, sep);
	for(size_t i=0;i<toks.size();++i)
	{
		toks[i]=trim(toks[i].c_str());

		if(toks[i].empty())
			continue;

		toks[i]=trim(toks[i]);

		if(toks[i].find("\\??\\")==0)
		{
			toks[i].erase(0, 4);
		}

		strupper(&toks[i]);

		exclude_dirs.push_back(sanitizePattern(toks[i]));
	}

	char* systemdrive = NULL;
	size_t bufferCount;
	if (_dupenv_s(&systemdrive, &bufferCount, "SystemDrive") == 0)
	{
		std::string excl = std::string(systemdrive) + "\\swapfile.sys";
		strupper(&excl);
		exclude_dirs.push_back(sanitizePattern(excl));
	}
	free(systemdrive);
#endif
}

void IndexThread::addHardExcludes(std::vector<std::string>& exclude_dirs)
{
#ifdef __linux__
	exclude_dirs.push_back("/proc/*");
	exclude_dirs.push_back("/dev/*");
	exclude_dirs.push_back("/sys/*");
	exclude_dirs.push_back("*/.datto_3d41c58e-6724-4d47-8981-11c766a08a24_:");
	exclude_dirs.push_back("*/.overlay_2fefd007-3e48-4162-b2c6-45ccdda22f37_:");
#endif

#ifdef _WIN32
	exclude_dirs.push_back(sanitizePattern(":\\SYSTEM VOLUME INFORMATION\\URBCT.DAT"));
	exclude_dirs.push_back(sanitizePattern(":\\SYSTEM VOLUME INFORMATION\\*{3808876B-C176-4E48-B7AE-04046E6CC752}*"));
#endif
}

void IndexThread::handleHardLinks(const std::string& bpath, const std::string& vsspath, const std::string& normalized_volume)
{
#ifdef _WIN32
	std::string prefixedbpath=os_file_prefix(bpath);
	std::wstring tvolume;
	tvolume.resize(prefixedbpath.size()+100);
	DWORD cchBufferLength=static_cast<DWORD>(tvolume.size());
	BOOL b=GetVolumePathNameW(Server->ConvertToWchar(prefixedbpath).c_str(), &tvolume[0], cchBufferLength);
	if(!b)
	{
		VSSLog("Error getting volume path for "+bpath, LL_WARNING);
		return;
	}

	std::wstring tvssvolume;
	tvssvolume.resize(vsspath.size()+100);
	cchBufferLength=static_cast<DWORD>(tvssvolume.size());
	b=GetVolumePathNameW(Server->ConvertToWchar(vsspath).c_str(), &tvssvolume[0], cchBufferLength);
	if(!b)
	{
		VSSLog("Error getting volume path for "+vsspath, LL_WARNING);
		return;
	}

	std::string vssvolume=Server->ConvertFromWchar(tvssvolume.c_str());

	std::string volume=strlower(Server->ConvertFromWchar(tvolume.c_str()));

	if(volume.find("\\\\?\\")==0)
		volume.erase(0, 4);

	std::vector<std::string> additional_changed_dirs;
	std::vector<std::string> additional_open_files;

	std::string prev_path;

	for(size_t i=0;i<changed_dirs.size();++i)
	{
		std::string vsstpath;
		{
			std::string tpath=changed_dirs[i];

			if(tpath.find(volume)!=0)
			{
				continue;
			}
			vsstpath=vssvolume+tpath.substr(volume.size());
		}

		if(!prev_path.empty() && prev_path==vsstpath)
		{
			continue;
		}
		else
		{
			prev_path = vsstpath;
		}

		bool has_error;
		std::vector<SFile> files = getFilesWin(os_file_prefix(vsstpath), &has_error, false, false, (index_flags & EBackupDirFlag_OneFilesystem)>0);
		std::vector<std::string> changed_files;
		bool looked_up_changed_files=false;

		if(has_error)
		{
			VSSLog("Cannot open directory "+vsstpath+" to handle hard links", LL_DEBUG);
		}

		const size_t& cdir_idx = i;


		for(size_t i=0;i<files.size();++i)
		{
			if(files[i].isdir)
			{
				continue;
			}

			std::string fn=vsstpath+files[i].name;
			HANDLE hFile = CreateFileW(Server->ConvertToWchar(os_file_prefix(fn)).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

			if(hFile==INVALID_HANDLE_VALUE)
			{
				VSSLog("Cannot open file "+fn+" to read the file attributes", LL_INFO);
			}
			else
			{
				FILE_STANDARD_INFO fileInformation;
				BOOL b= GetFileInformationByHandleEx(hFile, FileStandardInfo, &fileInformation, sizeof(fileInformation));

				if(!b)
				{
					VSSLog("Error getting file information of "+fn, LL_INFO);
					CloseHandle(hFile);
				}
				else if(fileInformation.NumberOfLinks>1)
				{
					CloseHandle(hFile);

					uint128 frn = getFrn(fn);


					SHardlinkKey hlkey = { normalized_volume, static_cast<int64>(frn.highPart), static_cast<int64>(frn.lowPart) };
					bool add_hardlinks_db = std::find(modify_hardlink_buffer_keys.begin(),
						modify_hardlink_buffer_keys.end(), hlkey) == modify_hardlink_buffer_keys.end();

					if (frn != uint128()
						&& add_hardlinks_db)
					{
						addResetHardlink(normalized_volume, frn.highPart, frn.lowPart);
					}
					

					bool file_is_open = std::binary_search(open_files.begin(),
						open_files.end(), changed_dirs[cdir_idx]+strlower(files[i].name));

					std::wstring outBuf;
					DWORD stringLength=4096;
					outBuf.resize(stringLength);
					HANDLE hFn=FindFirstFileNameW(Server->ConvertToWchar(os_file_prefix(fn)).c_str(), 0, &stringLength, &outBuf[0]);

					if(hFn==INVALID_HANDLE_VALUE && GetLastError()==ERROR_MORE_DATA)
					{
						outBuf.resize(stringLength);
						hFn=FindFirstFileNameW(Server->ConvertToWchar(os_file_prefix(fn)).c_str(), 0, &stringLength, &outBuf[0]);
					}

					if(hFn==INVALID_HANDLE_VALUE)
					{
						VSSLog("Error reading hard link names of "+fn, LL_INFO);
					}
					else
					{
						std::string nfn = strlower(Server->ConvertFromWchar(std::wstring(outBuf.begin(), outBuf.begin()+stringLength-1)));
						std::string vssnfn;
						if (nfn[0] == '\\')
						{
							vssnfn = vssvolume + nfn.substr(1);
							nfn = volume + nfn.substr(1);
						}
						else
						{
							vssnfn = vssvolume + nfn;
							nfn = volume + nfn;
						}

						std::string ndir=ExtractFilePath(nfn, os_file_sep())+os_file_sep();

						if (add_hardlinks_db)
						{
							uint128 dir_frn = getFrn(ExtractFilePath(vssnfn, os_file_sep()));

							if (dir_frn != uint128()
								&& frn != uint128())
							{
								addHardLink(normalized_volume, frn.highPart, frn.lowPart, dir_frn.highPart, dir_frn.lowPart);
							}
						}
					
						if(!std::binary_search(changed_dirs.begin(), changed_dirs.end(), ndir) )
						{
							additional_changed_dirs.push_back(ndir);
						}

						if(file_is_open &&
							std::find(additional_open_files.begin(), additional_open_files.end(), nfn)==
								additional_open_files.end())
						{
							additional_open_files.push_back(nfn);
						}

						do
						{
							b = FindNextFileNameW(hFn, &stringLength, &outBuf[0]);

							if(!b && GetLastError()==ERROR_MORE_DATA)
							{
								outBuf.resize(stringLength);
								b = FindNextFileNameW(hFn, &stringLength, &outBuf[0]);
							}

							if(!b && GetLastError()!=ERROR_HANDLE_EOF)
							{
								VSSLog("Error reading (2) hard link names of "+fn, LL_INFO);
							}
							else if(b)
							{
								std::string nfn = strlower(std::string(outBuf.begin(), outBuf.begin()+stringLength-1));
								std::string vssnfn;
								if (nfn[0] == '\\')
								{
									vssnfn = vssvolume + nfn.substr(1);
									nfn = volume + nfn.substr(1);
								}
								else
								{
									vssnfn = vssvolume + nfn;
									nfn = volume + nfn;
								}

								std::string ndir=ExtractFilePath(nfn, os_file_sep())+os_file_sep();

								if (add_hardlinks_db)
								{
									uint128 dir_frn = getFrn(ExtractFilePath(vssnfn, os_file_sep()));

									if (dir_frn != uint128()
										&& frn != uint128())
									{
										addHardLink(normalized_volume, frn.highPart, frn.lowPart, dir_frn.highPart, dir_frn.lowPart);
									}
								}
														
								if(!std::binary_search(changed_dirs.begin(), changed_dirs.end(), ndir))
								{
									additional_changed_dirs.push_back(ndir);
								}

								if(file_is_open &&
									std::find(additional_open_files.begin(), additional_open_files.end(), nfn)==
									additional_open_files.end())
								{
									additional_open_files.push_back(nfn);
								}
							}
						}
						while(b);

						FindClose(hFn);
					}
				}
				else
				{
					CloseHandle(hFile);
				}
			}
		}
	}

	if (!additional_changed_dirs.empty())
	{
		changed_dirs.insert(changed_dirs.end(), additional_changed_dirs.begin(), additional_changed_dirs.end());
		std::sort(changed_dirs.begin(), changed_dirs.end());
	}

	if (!additional_open_files.empty())
	{
		open_files.insert(open_files.end(), additional_open_files.begin(), additional_open_files.end());
		std::sort(open_files.begin(), open_files.end());
	}
#endif
}

void IndexThread::enumerateHardLinks(const std::string& volume, const std::string& vssvolume, const std::string & vsspath)
{
#ifdef _WIN32

	uint128 frn = getFrn(vsspath);

	if (frn == uint128())
	{
		return;
	}

	SHardlinkKey hlkey = { volume, static_cast<int64>(frn.highPart), static_cast<int64>(frn.lowPart) };
	if (cd->hasHardLink(volume, frn.highPart, frn.lowPart).exists
		|| std::find(modify_hardlink_buffer_keys.begin(), modify_hardlink_buffer_keys.end(), hlkey)!= modify_hardlink_buffer_keys.end() )
	{
		return;
	}

	std::wstring outBuf;
	DWORD stringLength = 4096;
	outBuf.resize(stringLength);
	HANDLE hFn = FindFirstFileNameW(Server->ConvertToWchar(os_file_prefix(vsspath)).c_str(), 0, &stringLength, &outBuf[0]);

	if (hFn == INVALID_HANDLE_VALUE && GetLastError() == ERROR_MORE_DATA)
	{
		outBuf.resize(stringLength);
		hFn = FindFirstFileNameW(Server->ConvertToWchar(os_file_prefix(vsspath)).c_str(), 0, &stringLength, &outBuf[0]);
	}

	if (hFn == INVALID_HANDLE_VALUE)
	{
		VSSLog("Error reading hard link names of " + vsspath+". "+os_last_error_str(), LL_ERROR);
	}
	else
	{
		std::string nfn = strlower(Server->ConvertFromWchar(std::wstring(outBuf.begin(), outBuf.begin() + stringLength - 1)));
		if (nfn[0] == '\\')
			nfn = vssvolume + nfn;
		else
			nfn = vssvolume + os_file_sep() + nfn;

		std::string ndir = ExtractFilePath(nfn, os_file_sep());

		uint128 dir_frn = getFrn(ndir);

		if (dir_frn != uint128())
		{
			addHardLink(volume, frn.highPart, frn.lowPart, dir_frn.highPart, dir_frn.lowPart);
		}

		BOOL b;

		do
		{
			b = FindNextFileNameW(hFn, &stringLength, &outBuf[0]);

			if (!b && GetLastError() == ERROR_MORE_DATA)
			{
				outBuf.resize(stringLength);
				b = FindNextFileNameW(hFn, &stringLength, &outBuf[0]);
			}

			if (!b && GetLastError() != ERROR_HANDLE_EOF)
			{
				VSSLog("Error reading (2) hard link names of " + vsspath +". "+os_last_error_str(), LL_INFO);
			}
			else if (b)
			{
				std::string nfn = strlower(std::string(outBuf.begin(), outBuf.begin() + stringLength - 1));
				if (nfn[0] == '\\')
					nfn = vssvolume + nfn;
				else
					nfn = vssvolume + os_file_sep() + nfn;

				std::string ndir = ExtractFilePath(nfn, os_file_sep());

				dir_frn = getFrn(ndir);

				if (dir_frn != uint128())
				{
					addHardLink(volume, frn.highPart, frn.lowPart, dir_frn.highPart, dir_frn.lowPart);
				}
			}
		} while (b);

		FindClose(hFn);
	}
#endif //_WIN32
}

void IndexThread::addResetHardlink(const std::string & volume, int64 frn_high, int64 frn_low)
{
	if (modify_hardlink_buffer_keys.size() > 1000
		|| modify_hardlink_buffer.size() > 10000)
	{
		commitModifyHardLinks();
	}

	SHardlinkKey key = { volume, frn_high, frn_low };
	modify_hardlink_buffer_keys.push_back(key);
}

void IndexThread::addHardLink(const std::string & volume, int64 frn_high, int64 frn_low, int64 parent_frn_high, int64 parent_frn_low)
{
	SHardlinkKey key = { volume, frn_high, frn_low };
	SHardlink hl = { key, parent_frn_high, parent_frn_low };
	modify_hardlink_buffer.push_back(hl);
}

void IndexThread::commitModifyHardLinks()
{
	DBScopedWriteTransaction transaction(db);

	for (size_t i = 0; i < modify_hardlink_buffer_keys.size(); ++i)
	{
		cd->resetHardlink(modify_hardlink_buffer_keys[i].volume, modify_hardlink_buffer_keys[i].frn_high, modify_hardlink_buffer_keys[i].frn_low);
	}

	modify_hardlink_buffer_keys.clear();

	for (size_t i = 0; i < modify_hardlink_buffer.size(); ++i)
	{
		cd->addHardlink(modify_hardlink_buffer[i].key.volume, modify_hardlink_buffer[i].key.frn_high, modify_hardlink_buffer[i].key.frn_low,
			modify_hardlink_buffer[i].parent_frn_high, modify_hardlink_buffer[i].parent_frn_low);
	}

	modify_hardlink_buffer.clear();
}

#ifdef _WIN32
uint128 IndexThread::getFrn(const std::string & fn)
{
	HANDLE hFile = CreateFileW(Server->ConvertToWchar(os_file_prefix(fn)).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

	if (hFile == INVALID_HANDLE_VALUE)
	{
		VSSLog("Cannot open file " + fn + " to read the FRN. "+os_last_error_str(), LL_ERROR);
		return uint128();
	}
	else
	{
		BY_HANDLE_FILE_INFORMATION fileInformation;
		BOOL b = GetFileInformationByHandle(hFile, &fileInformation);

		CloseHandle(hFile);

		if (b)
		{
			LARGE_INTEGER frn;
			frn.LowPart = fileInformation.nFileIndexLow;
			frn.HighPart = fileInformation.nFileIndexHigh;

			//TODO: Handle ReFS 128-bit FRN
			return uint128(frn.QuadPart);
		}
		else
		{
			VSSLog("Cannot get FRN of " + fn +". "+os_last_error_str(), LL_ERROR);
			return uint128();
		}
	}
}
#endif

std::string IndexThread::escapeListName( const std::string& listname )
{
	std::string ret;
	ret.reserve(listname.size());
	for(size_t i=0;i<listname.size();++i)
	{
		if(listname[i]=='"')
		{
			ret+="\\\"";
		}
		else if(listname[i]=='\\')
		{
			ret+="\\\\";
		}
		else
		{
			ret+=listname[i];
		}
	}
	return ret;
}

std::string IndexThread::getShaBinary(const std::string& fn)
{
	VSSLog("Hashing file \"" + fn + "\"", LL_DEBUG);

	if (sha_version == 256)
	{
		HashSha256 hash_256;
		if (!getShaBinary(fn, hash_256, false))
		{
			return std::string();
		}

		return hash_256.finalize();
	}
	else if (sha_version == 528)
	{
		TreeHash treehash(index_hdat_file.get() == NULL ? NULL : client_hash.get());
		if (!getShaBinary(fn, treehash, index_hdat_file.get() != NULL))
		{
			return std::string();
		}

		return treehash.finalize();
	}
	else
	{
		HashSha512 hash_512;
		if (!getShaBinary(fn, hash_512, false))
		{
			return std::string();
		}

		return hash_512.finalize();
	}
}

bool IndexThread::getShaBinary( const std::string& fn, IHashFunc& hf, bool with_cbt)
{
	return client_hash->getShaBinary(fn, hf, with_cbt);
}

bool IndexThread::backgroundBackupsEnabled(const std::string& clientsubname)
{
	std::string settings_fn = "urbackup/data/settings.cfg";

	if(!clientsubname.empty())
	{
		settings_fn = "urbackup/data/settings_"+conv_filename(clientsubname)+".cfg";
	}

	std::auto_ptr<ISettingsReader> curr_settings(Server->createFileSettingsReader(settings_fn));
	if(curr_settings.get()!=NULL)
	{
		std::string background_backups;
		if(curr_settings->getValue("background_backups", &background_backups)
			|| curr_settings->getValue("background_backups_def", &background_backups) )
		{
			return background_backups!="false";
		}
	}
	return true;
}

void IndexThread::writeTokens()
{
	std::auto_ptr<ISettingsReader> access_keys(
		Server->createFileSettingsReader("urbackup/access_keys.properties"));

	std::string access_keys_data;
	std::vector<std::string> keys
		= access_keys->getKeys();


	bool has_server_key=false;
	std::string curr_key;
	int64 curr_key_age = 0;
	for(size_t i=0;i<keys.size();++i)
	{
		if(keys[i]=="key."+starttoken)
		{
			has_server_key=true;
			curr_key=access_keys->getValue(keys[i], std::string());
			curr_key_age = access_keys->getValue("key_age."+starttoken, Server->getTimeSeconds());
		}
		else if(keys[i]!="key."+starttoken
			&& keys[i]!="last.key."+starttoken)
		{
			access_keys_data+=keys[i]+"="+
				access_keys->getValue(keys[i], std::string())+"\n";
		}
	}

	bool modified_file = false;
	std::string last_key;

	if(!has_server_key || (Server->getTimeSeconds()-curr_key_age)>7*24*60*60)
	{
		if (has_server_key)
		{
			last_key = curr_key;
		}
		curr_key = Server->secureRandomString(32);
		curr_key_age = Server->getTimeSeconds();
		modified_file=true;
	}

	if (!last_key.empty())
	{
		access_keys_data += "last.key." + starttoken + "=" +
			last_key + "\n";
	}

	access_keys_data+="key."+starttoken+"="+
		curr_key+"\n";

	access_keys_data+="key_age."+starttoken+"="+
		convert(curr_key_age)+"\n";

	if(modified_file)
	{
		write_file_only_admin(access_keys_data, "urbackup/access_keys.properties.new");
		os_rename_file("urbackup/access_keys.properties.new", "urbackup/access_keys.properties");
	}	

	tokens::write_tokens();
	std::vector<ClientDAO::SToken> tokens = cd->getFileAccessTokens();

	std::string ids;
	std::string uids;
	std::string real_uids;
	for(size_t i=0;i<tokens.size();++i)
	{
		if(!ids.empty())
		{
			ids+=",";
		}
		ids+=convert(tokens[i].id);

		if(tokens[i].is_user)
		{
			if(!uids.empty())
			{
				uids+=",";
			}
			uids+=convert(tokens[i].id);
		}

		if(tokens[i].is_user &&
				tokens[i].is_user!=ClientDAO::c_is_system_user)
		{
			if(!real_uids.empty())
			{
				real_uids+=",";
			}
			real_uids+=convert(tokens[i].id);
		}

	}

	std::string data="ids="+ids+"\n";
	data+="access_key="+curr_key+"\n";
	data+="uids="+uids+"\n";
	data+="real_uids="+real_uids+"\n";

	for(size_t i=0;i<tokens.size();++i)
	{
		data+=convert(tokens[i].id)+".accountname="+base64_encode_dash((tokens[i].accountname))+"\n";
		data+=convert(tokens[i].id)+".token="+(tokens[i].token)+"\n";

		if(tokens[i].is_user)
		{
			std::vector<int> groups = cd->getGroupMembership(static_cast<int>(tokens[i].id));

			std::string gids;

			for(size_t j=0;j<groups.size();++j)
			{
				if(!gids.empty())
				{
					gids+=",";
				}
				gids+=convert(groups[j]);
			}

			data+=convert(tokens[i].id)+".gids="+gids+"\n";
		}
	}


	write_file_only_admin(data, "urbackup"+os_file_sep()+"data"+os_file_sep()+"tokens_"+starttoken+".properties");
}

void IndexThread::writeDir(std::fstream& out, const std::string& name, bool with_change, uint64 change_identicator, const std::string& extra)
{
	out << "d\"" << escapeListName((name)) << "\"";

	if(with_change)
	{
		out << " 0 " << static_cast<int64>(change_identicator);
	}

	if(!extra.empty())
	{
		if(extra[0]=='&')
			out << "#" << extra.substr(1) << "\n";
		else
			out << extra << "\n";
	}
	else
	{
		out << "\n";
	}

	++file_id;;
}

std::string IndexThread::execute_script(const std::string& cmd, const std::string& args)
{
	std::string output;
	int rc = os_popen("\""+cmd+"\"" + (args.empty() ? "" : (" "+args)), output);

	if(rc!=0)
	{
		Server->Log("Script "+cmd+" had error (code "+convert(rc)+"). Not using this script list.", LL_ERROR);
		return std::string();
	}

	return output;
}

int IndexThread::execute_preimagebackup_hook(bool incr, std::string server_token)
{
	std::string script_name;
#ifdef _WIN32
	script_name = Server->getServerWorkingDir() + "\\preimagebackup.bat";
#else
	script_name = SYSCONFDIR "/urbackup/preimagebackup";
#endif

	return execute_hook(script_name, incr, server_token, NULL);
}

bool IndexThread::addBackupScripts(std::fstream& outfile)
{
	if(!scripts.empty())
	{
		outfile << "d\"urbackup_backup_scripts\"\n";
		++file_id;

		for(size_t i=0;i<scripts.size();++i)
		{
			int64 rndnum=Server->getRandomNumber()<<30 | Server->getRandomNumber();
			outfile << "f\"" << escapeListName(scripts[i].outputname) << "\" " << scripts[i].size << " " << rndnum;
			++file_id;

			if (!scripts[i].orig_path.empty())
			{
				std::string orig_path = scripts[i].orig_path;
				if (orig_path.size() > 1 && orig_path[orig_path.size() - 1] == os_file_sep()[0])
				{
					orig_path.erase(orig_path.size() - 1, 1);
				}
				outfile << "#orig_path=" << EscapeParamString(orig_path) << "&orig_sep=" << EscapeParamString(os_file_sep());
			}
			
			outfile << "\n";
		}

		outfile << "u\n";
		++file_id;

		return true;
	}
	else
	{
		return false;
	}
}

void IndexThread::monitor_disk_failures()
{
#ifdef _WIN32
	std::vector<SFailedDisk> failed_disks = get_failed_disks();

	for (size_t i = 0; i < failed_disks.size(); ++i)
	{
		VSSLog("Disk \"" + failed_disks[i].name + "\" has status \"" + failed_disks[i].status + "\" and may need replacement" +
			(failed_disks[i].status_info.empty() ? "" : "(Further info: \"" + failed_disks[i].status_info + "\")"), LL_WARNING);
	}
#endif
}

int IndexThread::get_db_tgroup()
{
	if (index_flags & EBackupDirFlag_ShareHashes)
	{
		return 0;
	}
	else
	{
		return index_group + 1;
	}
}

bool IndexThread::nextLastFilelistItem(SFile& data, str_map* extra, bool with_up)
{
	if (last_filelist.get() == NULL
		|| last_filelist->f==NULL)
	{
		return false;
	}

	if (last_filelist->buf.empty())
	{
		last_filelist->buf.resize(4096);
		last_filelist->buf_pos = std::string::npos;
	}

	while (true)
	{
		if (last_filelist->buf_pos == last_filelist->buf.size()
			|| last_filelist->buf_pos==std::string::npos)
		{
			if (last_filelist->buf_pos == last_filelist->buf.size())
			{
				last_filelist->read_pos += last_filelist->buf.size();
			}

			last_filelist->buf_pos = 0;
			bool has_read_error = false;
			_u32 read = last_filelist->f->Read(last_filelist->buf.data(),
				static_cast<_u32>(last_filelist->buf.size()), &has_read_error);
			if (has_read_error)
			{
				Server->Log("Error reading from last file list", LL_ERROR);

				last_filelist.reset();
				index_follow_last = false;
				return false;
			}

			if (read == 0)
			{
				last_filelist.reset();
				index_follow_last = false;
				return false;
			}

			if (read < last_filelist->buf.size())
			{
				last_filelist->buf.resize(read);
			}
		}
		else
		{
			if (last_filelist->parser.nextEntry(last_filelist->buf[last_filelist->buf_pos++], data, extra))
			{
				handleLastFilelistDepth(data);
				last_filelist->item_pos = last_filelist->read_pos + last_filelist->buf_pos;

				if (!with_up && data.isdir && data.name == "..")
				{
					return nextLastFilelistItem(data, extra, with_up);
				}

				return true;
			}
		}
	}
}

void IndexThread::addFromLastUpto(const std::string& fname, bool isdir, size_t depth, bool finish, std::fstream &outfile)
{
	if (!index_follow_last || last_filelist.get()==NULL)
	{
		return;
	}

	if (last_filelist->item.name.empty())
	{
		if (!nextLastFilelistItem(last_filelist->item, &last_filelist->extra, false))
		{
			return;
		}
	}

	assert(depth >= last_filelist->depth);
	
	do
	{
		if (!finish
			&& ((last_filelist->item.name > fname
				&& last_filelist->item.isdir == isdir
				&& depth == last_filelist->depth)
				|| depth > last_filelist->depth))
		{
			return;
		}

		if (!finish
			&& last_filelist->item.name == fname
			&& last_filelist->item.isdir == isdir
			&& depth == last_filelist->depth)
		{
			nextLastFilelistItem(last_filelist->item, &last_filelist->extra, false);
			return;
		}

		if (index_keep_files)
		{
			if (last_filelist->item.isdir)
			{
				addDirFromLast(outfile);
			}
			else
			{
				addFileFromLast(outfile);
			}
		}
	} while (nextLastFilelistItem(last_filelist->item, &last_filelist->extra, false));
}

void IndexThread::addFromLastLiftDepth(size_t depth, std::fstream & outfile)
{
	if (!index_follow_last || last_filelist.get() == NULL)
	{
		return;
	}

	if (last_filelist->item.name.empty())
	{
		if (!nextLastFilelistItem(last_filelist->item, &last_filelist->extra, false))
		{
			return;
		}
	}

	while (last_filelist->depth > depth)
	{
		if (index_keep_files)
		{
			if (last_filelist->item.isdir)
			{
				addDirFromLast(outfile);
			}
			else
			{
				addFileFromLast(outfile);
			}
		}

		if (!nextLastFilelistItem(last_filelist->item, &last_filelist->extra, false))
		{
			return;
		}
	}
}

void IndexThread::addDirFromLast(std::fstream & outfile)
{
	size_t curr_depth = last_filelist->depth;
	do
	{
		if (last_filelist->item.isdir)
		{
			std::string str_extra;
			for (str_map::iterator it = last_filelist->extra.begin(); it != last_filelist->extra.end(); ++it)
			{
				str_extra += "&" + it->first + "=" + EscapeParamString(it->second);
			}

			writeDir(outfile, last_filelist->item.name, with_orig_path, last_filelist->item.last_modified, str_extra);
		}
		else
		{
			addFileFromLast(outfile);
		}		
	} while (last_filelist->depth_next > curr_depth
		&& nextLastFilelistItem(last_filelist->item, &last_filelist->extra, true));
}

void IndexThread::addFileFromLast(std::fstream & outfile)
{
	std::string str_extra;
	for (str_map::iterator it = last_filelist->extra.begin(); it != last_filelist->extra.end(); ++it)
	{
		str_extra += "&" + it->first + "=" + EscapeParamString(it->second);
	}

	outfile << "f\"" << escapeListName(last_filelist->item.name) << "\" " << last_filelist->item.size << " " << last_filelist->item.last_modified;

	if (!str_extra.empty())
	{
		str_extra[0] = '#';
		outfile << str_extra;
	}

	outfile << "\n";
	++file_id;
}

bool IndexThread::handleLastFilelistDepth(SFile& data)
{
	last_filelist->depth = last_filelist->depth_next;

	if (data.isdir)
	{
		if (data.name == "..")
		{
			if (last_filelist->depth_next == 0)
			{
				return false;
			}

			--last_filelist->depth_next;
		}
		else
		{
			++last_filelist->depth_next;
		}
	}

	return true;
}

bool IndexThread::volIsEnabled(std::string settings_val, std::string volume)
{
	settings_val = strlower(trim(settings_val));

	if (settings_val == "all")
	{
		return true;
	}

	if (volume.size() == 2 && volume[1] == ':')
	{
		volume.resize(1);
	}

	volume = strlower(volume);

	std::vector<std::string> vols;
	TokenizeMail(settings_val, vols, ",;");

	for (size_t i = 0; i < vols.size(); ++i)
	{
		if (vols[i].size() == 2 && vols[i][1] == ':')
		{
			vols[i].resize(1);
		}

		if (vols[i] == volume)
		{
			return true;
		}
	}

	return false;
}

bool IndexThread::cbtIsEnabled(std::string clientsubname, std::string volume)
{
	std::string settings_fn = "urbackup/data/settings.cfg";

	if (!clientsubname.empty())
	{
		settings_fn = "urbackup/data/settings_" + conv_filename(clientsubname) + ".cfg";
	}

	std::auto_ptr<ISettingsReader> curr_settings(Server->createFileSettingsReader(settings_fn));
	if (curr_settings.get() != NULL)
	{
		std::string cbt_volumes;
		if (curr_settings->getValue("cbt_volumes", &cbt_volumes)
			|| curr_settings->getValue("cbt_volumes_def", &cbt_volumes))
		{
			return volIsEnabled(cbt_volumes, volume);
		}
	}
	return true;
}

bool IndexThread::crashPersistentCbtIsEnabled(std::string clientsubname, std::string volume)
{
	std::string settings_fn = "urbackup/data/settings.cfg";

	if (!clientsubname.empty())
	{
		settings_fn = "urbackup/data/settings_" + conv_filename(clientsubname) + ".cfg";
	}

	std::auto_ptr<ISettingsReader> curr_settings(Server->createFileSettingsReader(settings_fn));
	if (curr_settings.get() != NULL)
	{
		std::string cbt_crash_persistent_volumes;
		if (curr_settings->getValue("cbt_crash_persistent_volumes", &cbt_crash_persistent_volumes)
			|| curr_settings->getValue("cbt_crash_persistent_volumes_def", &cbt_crash_persistent_volumes))
		{
			return volIsEnabled(cbt_crash_persistent_volumes, volume);
		}
	}
	return false;
}

#ifdef _WIN32
#define URBT_BLOCKSIZE (512 * 1024)
#define URBT_MAGIC "~urbackupcbt!"
#define URBT_MAGIC_SIZE 13

typedef struct _URBCT_BITMAP_DATA
{
	DWORD BitmapSize;
	ULONG SectorSize;
	BYTE Bitmap[1];
} URBCT_BITMAP_DATA, *PURBCT_BITMAP_DATA;

#define IOCTL_URBCT_RESET_START CTL_CODE(FILE_DEVICE_DISK, 3240, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_URBCT_RETRIEVE_BITMAP CTL_CODE(FILE_DEVICE_DISK, 3241, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_URBCT_RESET_FINISH CTL_CODE(FILE_DEVICE_DISK, 3242, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_URBCT_MARK_ALL CTL_CODE(FILE_DEVICE_DISK, 3245, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_URBCT_APPLY_BITMAP CTL_CODE(FILE_DEVICE_DISK, 3246, METHOD_BUFFERED, FILE_READ_DATA)

namespace
{
	class ScopedCloseWindowsHandle
	{
	public:
		ScopedCloseWindowsHandle(HANDLE h)
			: h(h)
		{}

		~ScopedCloseWindowsHandle() {
			if (h != INVALID_HANDLE_VALUE) {
				CloseHandle(h);
			}
		}
	private:
		HANDLE h;
	};

	PURBCT_BITMAP_DATA readBitmap(std::string fn, std::vector<char>& data)
	{
		std::auto_ptr<IFile> f(Server->openFile(fn, MODE_READ));

		if (f.get() == NULL)
		{
			Server->Log("Error opening file " + fn + ". " + os_last_error_str(), LL_ERROR);
			return NULL;
		}

		if (f->Size() < 16)
		{
			return NULL;
		}

		std::string md5sum = f->Read(16);

		if (md5sum.size() != 16)
		{
			return NULL;
		}

		data.resize(f->Size()-16);

		size_t read = 0;
		while (read < data.size())
		{
			bool has_read_error = false;
			_u32 radd = f->Read(data.data() + read, static_cast<_u32>(data.size() - read), &has_read_error);

			read += radd;

			if (has_read_error || radd==0)
			{
				Server->Log("Error reading from file " + fn + ". " + os_last_error_str(), LL_ERROR);
				return NULL;
			}
		}

		MD5 md;
		md.update(reinterpret_cast<unsigned char*>(data.data()), static_cast<_u32>(data.size()));
		md.finalize();

		if (memcmp(md5sum.data(), md.raw_digest_int(), md5sum.size()) != 0)
		{
			Server->Log("Checksum of " + fn + " wrong", LL_ERROR);
			return NULL;
		}

		return reinterpret_cast<PURBCT_BITMAP_DATA>(data.data());
	}

	bool mergeBitmap(PURBCT_BITMAP_DATA src, PURBCT_BITMAP_DATA dst)
	{
		if (src->SectorSize != dst->SectorSize)
		{
			return false;
		}

		DWORD curr_byte = 0;
		for (DWORD i = 0; i < src->BitmapSize && i<dst->BitmapSize; i += src->SectorSize)
		{
			for (DWORD j = i + URBT_MAGIC_SIZE; j < i + src->SectorSize; ++j)
			{
				dst->Bitmap[j] |= src->Bitmap[j];
			}
		}

		return true;
	}

	bool readMergeBitmap(std::string fn, PURBCT_BITMAP_DATA bitmap)
	{
		if (!FileExists(fn))
		{
			Server->Log("Bitmap " + fn + " does not exist. Nothing to merge.", LL_DEBUG);
			return true;
		}

		PURBCT_BITMAP_DATA old_bitmap;
		std::vector<char> old_bitmap_data;

		old_bitmap = readBitmap(fn, old_bitmap_data);

		if (old_bitmap == NULL)
		{
			return false;
		}

		return mergeBitmap(old_bitmap, bitmap);
	}

	bool saveMergeBitmap(std::string fn, PURBCT_BITMAP_DATA bitmap)
	{
		PURBCT_BITMAP_DATA old_bitmap=NULL;
		std::vector<char> old_bitmap_data;

		if (FileExists(fn))
		{
			old_bitmap = readBitmap(fn, old_bitmap_data);

			if (old_bitmap == NULL)
			{
				return false;
			}
		}

		std::auto_ptr<IFile> f(Server->openFile(fn+".new", MODE_WRITE));

		if (f.get() == NULL)
		{
			Server->Log("Error creating file " + fn + ".new. " + os_last_error_str(), LL_ERROR);
			return false;
		}

		if (old_bitmap != NULL)
		{
			if (!mergeBitmap(bitmap, old_bitmap))
			{
				return false;
			}
		}
		else
		{
			old_bitmap = bitmap;
		}

		size_t dsize = old_bitmap->BitmapSize + sizeof(DWORD) + sizeof(ULONG);

		MD5 md;
		md.update(reinterpret_cast<unsigned char*>(old_bitmap), static_cast<_u32>(dsize));
		md.finalize();

		if (f->Write(reinterpret_cast<char*>(md.raw_digest_int()), 16) != 16)
		{
			Server->Log("Error writing bitmap checksum. " + os_last_error_str(), LL_ERROR);
			return false;
		}

		if (f->Write(reinterpret_cast<char*>(old_bitmap), static_cast<_u32>(dsize)) != dsize)
		{
			Server->Log("Error writing bitmap. " + os_last_error_str(), LL_ERROR);
			return false;
		}

		f->Sync();
		f.reset();

		if (!os_rename_file(fn + ".new", fn))
		{
			Server->Log("Error renaming " + fn + ".new to " + fn + ". " + os_last_error_str(), LL_ERROR);
			return false;
		}

		f.reset(Server->openFile(fn, MODE_RW));

		if (f.get() == NULL)
		{
			return false;
		}
		
		f->Sync();

		return true;
	}
}

#endif

bool IndexThread::prepareCbt(std::string volume)
{
#ifdef _WIN32
	if (!normalizeVolume(volume))
	{
		VSSLog("Error normalizing volume. Input \"" + volume + "\" (1)", LL_ERROR);
		return false;
	}

	if (!cbtIsEnabled(std::string(), volume))
	{
		VSSLog("CBT not enabled for volume \"" + volume + "\"", LL_INFO);
		return false;
	}

	HANDLE hVolume = CreateFileA(("\\\\.\\" + volume).c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);

	if (hVolume == INVALID_HANDLE_VALUE)
	{
		VSSLog("Error opening volume " + volume + ". " + os_last_error_str(), LL_ERROR);
		return false;
	}

	ScopedCloseWindowsHandle hclose(hVolume);

	DWORD bytesReturned;
	BOOL b = DeviceIoControl(hVolume, IOCTL_URBCT_RESET_START, NULL, 0, NULL, 0, &bytesReturned, NULL);

	if (!b)
	{
		DWORD lasterr = GetLastError();

		std::string errmsg;
		int64 err = os_last_error(errmsg);
		int ll = lasterr != ERROR_INVALID_FUNCTION ? LL_ERROR : LL_DEBUG;
		VSSLog("Preparing change block tracking reset for volume " + volume + " failed: " + errmsg + " (code: " + convert(err) + ")", ll);
		
		if ( (lasterr == ERROR_INVALID_FUNCTION
				&& os_get_file_type("urbctctl.exe")!=0 )
			|| lasterr !=ERROR_INVALID_FUNCTION )
		{
			if (cbtIsEnabled(std::string(), volume))
			{
				enableCbtVol(volume, true);
			}
		}
	}

	return b == TRUE;
#else
	return false;
#endif
}

bool IndexThread::normalizeVolume(std::string & volume)
{
#ifdef _WIN32
	if (volume.empty())
	{
		return false;
	}

	if (volume[volume.size() - 1] == os_file_sep()[0])
	{
		volume = volume.substr(0, volume.size() - 1);
	}

	if (volume.size() > 2)
	{
		volume = getVolPath(volume);

		if (volume.empty())
		{
			return false;
		}
	}
	else if (volume.size() == 1)
	{
		volume += ":";
	}

	if (!volume.empty()
		&& volume[volume.size() - 1] == os_file_sep()[0])
	{
		volume = volume.substr(0, volume.size() - 1);
	}
#endif

	return true;
}

bool IndexThread::finishCbt(std::string volume, int shadow_id, std::string snap_volume, bool for_image_backup)
{
#ifdef _WIN32
	if (!normalizeVolume(volume))
	{
		VSSLog("Error normalizing volume. Input \""+volume+"\" (2)", LL_ERROR);
		return false;
	}

	HANDLE hVolume = CreateFileA(("\\\\.\\" + volume).c_str(), GENERIC_READ|GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);

	if (hVolume == INVALID_HANDLE_VALUE)
	{
		VSSLog("Error opening volume "+volume+". "+os_last_error_str(), LL_ERROR);
		return false;
	}

	ScopedDisableBackgroundPrio disable_background_prio(background_prio.get());

	ScopedCloseWindowsHandle hclose(hVolume);

	if (FlushFileBuffers(hVolume) == FALSE)
	{
		std::string errmsg;
		int64 err = os_last_error(errmsg);
		VSSLog("Flushing volume " + volume + " failed: " + errmsg + " (code: " + convert(err) + ")", LL_ERROR);
		return false;
	}

	HANDLE hSnapVolume = INVALID_HANDLE_VALUE;
	if (!snap_volume.empty())
	{
		hSnapVolume = CreateFileA(snap_volume.c_str(), GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);

		if (hSnapVolume == INVALID_HANDLE_VALUE)
		{
			VSSLog("Error opening volume snapshot of "+volume+" at " + snap_volume+ ". " + os_last_error_str(), LL_ERROR);
			return false;
		}
	}

	ScopedCloseWindowsHandle hclosesnap(hSnapVolume);

	GET_LENGTH_INFORMATION lengthInfo;
	DWORD retBytes;
	BOOL b = DeviceIoControl(hVolume, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &lengthInfo, sizeof(lengthInfo),
		&retBytes, NULL);

	if (!b)
	{
		std::string errmsg;
		int64 err = os_last_error(errmsg);
		VSSLog("Getting length information for volume " + volume + " failed: " + errmsg + " (code: " + convert(err) + ")", LL_ERROR);
		return false;
	}

	DISK_GEOMETRY disk_geometry = {};
	b = DeviceIoControl(hVolume, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &disk_geometry, sizeof(disk_geometry),
		&retBytes, NULL);

	if (!b)
	{
		std::string errmsg;
		int64 err = os_last_error(errmsg);
		VSSLog("Getting disk geometry of volume " + volume + " failed: " + errmsg + " (code: " + convert(err) + ")", LL_ERROR);
		return false;
	}
	

	ULONGLONG bitmapBlocks = lengthInfo.Length.QuadPart / URBT_BLOCKSIZE + (lengthInfo.Length.QuadPart%URBT_BLOCKSIZE == 0 ? 0 : 1);

	size_t bitmapBytesWoMagic = bitmapBlocks / 8 + (bitmapBlocks % 8 == 0 ? 0 : 1);

	DWORD bitmapSectorSize = disk_geometry.BytesPerSector - URBT_MAGIC_SIZE;

	size_t bitmapBytes = (bitmapBytesWoMagic / bitmapSectorSize)*disk_geometry.BytesPerSector 
		+ ((bitmapBytesWoMagic%bitmapSectorSize != 0) ? (URBT_MAGIC_SIZE + bitmapBytesWoMagic%bitmapSectorSize) : 0);

	std::vector<char> buf;
	buf.resize(2*sizeof(DWORD));

	DWORD bytesReturned;
	b = DeviceIoControl(hVolume, IOCTL_URBCT_RETRIEVE_BITMAP, NULL, 0, buf.data(), static_cast<DWORD>(buf.size()), &bytesReturned, NULL);

	if (!b && GetLastError() != ERROR_MORE_DATA)
	{
		std::string errmsg;
		int64 err = os_last_error(errmsg);
		VSSLog("Getting changed block data from volume " + volume + " failed: " + errmsg + " (code: " + convert(err) + ")", LL_ERROR);
		return false;
	}

	PURBCT_BITMAP_DATA bitmap_data = reinterpret_cast<PURBCT_BITMAP_DATA>(buf.data());

	if (bitmap_data->BitmapSize < bitmapBytes)
	{
		VSSLog("Did not track enough (volume resize?). Tracked " + convert((int)bitmap_data->BitmapSize) + " should track " + convert(bitmapBytes)+". "
			"CBT will disable itself once this area is written to and then a system restart will be needed to enable it again.", LL_INFO);
	}

	buf.resize(2*sizeof(DWORD) + bitmap_data->BitmapSize);

	b = DeviceIoControl(hVolume, IOCTL_URBCT_RETRIEVE_BITMAP, NULL, 0, buf.data(), static_cast<DWORD>(buf.size()), &bytesReturned, NULL);

	if (!b)
	{
		std::string errmsg;
		int64 err = os_last_error(errmsg);
		VSSLog("Getting changed block data from volume " + volume + " failed (2): " + errmsg + " (code: " + convert(err) + ")", LL_ERROR);
		return false;
	}

	std::vector<char> buf_snap;
	if (hSnapVolume != INVALID_HANDLE_VALUE)
	{
		buf_snap.resize(buf.size());

		b = DeviceIoControl(hSnapVolume, IOCTL_URBCT_RETRIEVE_BITMAP, NULL, 0, buf_snap.data(), static_cast<DWORD>(buf_snap.size()), &bytesReturned, NULL);

		if (!b)
		{
			std::string errmsg;
			int64 err = os_last_error(errmsg);
			VSSLog("Getting changed block data from shadow copy " + snap_volume + " failed: " + errmsg + " (code: " + convert(err) + ")", LL_ERROR);
			return false;
		}
	}

	bitmap_data = reinterpret_cast<PURBCT_BITMAP_DATA>(buf.data());
	char* urbackupcbt_magic = URBT_MAGIC;

	PURBCT_BITMAP_DATA snap_bitmap_data = reinterpret_cast<PURBCT_BITMAP_DATA>(buf_snap.data());

	DWORD RealBitmapSize = 0;

	int64 changed_bytes_sc = 0;

	for (DWORD i = 0; i < bitmap_data->BitmapSize; i+=bitmap_data->SectorSize)
	{
		if (memcmp(&bitmap_data->Bitmap[i], urbackupcbt_magic, URBT_MAGIC_SIZE) != 0)
		{
			VSSLog("UrBackup cbt magic wrong at pos "+convert((size_t)i), LL_ERROR);
			return false;
		}

		DWORD tr = (std::min)(bitmap_data->BitmapSize - i - URBT_MAGIC_SIZE, bitmap_data->SectorSize - URBT_MAGIC_SIZE);

		RealBitmapSize += tr;

		if(hSnapVolume!=INVALID_HANDLE_VALUE
			&& i + URBT_MAGIC_SIZE < snap_bitmap_data->BitmapSize)
		{
			if (memcmp(&snap_bitmap_data->Bitmap[i], urbackupcbt_magic, URBT_MAGIC_SIZE) != 0)
			{
				VSSLog("UrBackup cbt snap magic wrong at shadow copy bitmap pos " + convert((size_t)i), LL_ERROR);
				return false;
			}

			DWORD tr_snap = (std::min)(tr, snap_bitmap_data->BitmapSize - i - URBT_MAGIC_SIZE);

			for (DWORD j = i + URBT_MAGIC_SIZE; j < i + URBT_MAGIC_SIZE + tr_snap; ++j)
			{
				BYTE b = snap_bitmap_data->Bitmap[j];
				bitmap_data->Bitmap[j] |= b;

				while (b > 0)
				{
					if (b & 1)
					{
						changed_bytes_sc += URBT_BLOCKSIZE;
					}
					b >>= 1;
				}
			}
		}
	}

	if (hSnapVolume != INVALID_HANDLE_VALUE)
	{
		VSSLog("Change block tracking reports " + PrettyPrintBytes(changed_bytes_sc) + " have changed on shadow copy " + snap_volume, LL_DEBUG);

		b = DeviceIoControl(hVolume, IOCTL_URBCT_APPLY_BITMAP, buf_snap.data(), static_cast<DWORD>(buf_snap.size()), NULL, 0, &bytesReturned, NULL);

		if (!b)
		{
			std::string errmsg;
			int64 err = os_last_error(errmsg);
			VSSLog("Applying shadow copy changes to " + volume + " failed: " + errmsg + " (code: " + convert(err) + ")", LL_ERROR);
			return false;
		}
	}

	VSSLog("Change block tracking active on volume " + volume, LL_INFO);

	if (for_image_backup)
	{
		if (!saveMergeBitmap("urbackup\\hdat_file_" + conv_filename(strlower(volume)) + ".cbt", bitmap_data))
		{
			VSSLog("Error saving CBT bitmap for file backup", LL_ERROR);
			return false;
		}

		if (!readMergeBitmap("urbackup\\hdat_img_" + conv_filename(strlower(volume)) + ".cbt", bitmap_data))
		{
			VSSLog("Error reading last bitmap data for CBT for image backup", LL_ERROR);
			return false;
		}

		std::auto_ptr<IFsFile> hdat_img(ImageThread::openHdatF(volume, false));

		bool concurrent_active = false;

		if (hdat_img.get() == NULL)
		{
			hdat_img.reset(ImageThread::openHdatF(volume, true));

			if (hdat_img.get() != NULL)
			{
				concurrent_active = true;
			}
		}

		if (hdat_img.get() == NULL)
		{
			std::string errmsg;
			int64 err = os_last_error(errmsg);
			VSSLog("Cannot open image hash data file for change block tracking. " + errmsg + " (code: " + convert(err) + ")", LL_ERROR);
			return false;
		}

		hdat_img->Resize(sizeof(shadow_id) + RealBitmapSize * 8 * SHA256_DIGEST_SIZE);

		if (hdat_img->Write(0, reinterpret_cast<char*>(&shadow_id), sizeof(shadow_id)) != sizeof(shadow_id))
		{
			VSSLog("Error writing shadow id", LL_ERROR);
			return false;
		}

		{
			IScopedLock lock(cbt_shadow_id_mutex);
			cbt_shadow_ids[strlower(volume)] = shadow_id;
		}

		if (concurrent_active)
		{
			Server->wait(10000);
		}

		char zero_sha[SHA256_DIGEST_SIZE] = {};

		VSSLog("Zeroing image hash data of volume " + volume + "...", LL_DEBUG);

		int64 changed_bytes = 0;

		DWORD curr_byte = 0;
		for (DWORD i = 0; i < bitmap_data->BitmapSize; i += bitmap_data->SectorSize)
		{
			for (DWORD j = i + URBT_MAGIC_SIZE; j < i + bitmap_data->SectorSize; ++j)
			{
				BYTE ch = bitmap_data->Bitmap[j];

				if (ch == 0)
				{
					++curr_byte;
					continue;
				}

				for (DWORD bit = 0; bit < 8; ++bit)
				{
					if ((ch & (1 << bit)) > 0)
					{
						if (hdat_img->Write(sizeof(shadow_id) + (curr_byte * 8 + bit)*SHA256_DIGEST_SIZE, zero_sha, SHA256_DIGEST_SIZE) != SHA256_DIGEST_SIZE)
						{
							std::string errmsg;
							int64 err = os_last_error(errmsg);
							VSSLog("Errro zeroing image hash data. " + errmsg + " (code: " + convert(err) + ")", LL_ERROR);
							return false;
						}

						changed_bytes += URBT_BLOCKSIZE;
					}
				}

				++curr_byte;
			}
		}

		hdat_img->Sync();

		Server->deleteFile("urbackup\\hdat_img_" + conv_filename(strlower(volume)) + ".cbt");
	}
	else //!for_image_backup
	{
		if (!saveMergeBitmap("urbackup\\hdat_img_" + conv_filename(strlower(volume)) + ".cbt", bitmap_data))
		{
			VSSLog("Error saving CBT bitmap for image backup", LL_ERROR);
			return false;
		}

		if (!readMergeBitmap("urbackup\\hdat_file_" + conv_filename(strlower(volume)) + ".cbt", bitmap_data))
		{
			VSSLog("Error reading last bitmap data for CBT for image backup", LL_ERROR);
			return false;
		}

		{
			IScopedLock lock(cbt_shadow_id_mutex);
			++index_hdat_sequence_ids[strlower(volume)];
		}
		
		std::auto_ptr<IFsFile> hdat_file(Server->openFile("urbackup\\hdat_file_" + conv_filename(strlower(volume)) + ".dat", MODE_RW_CREATE_DELETE));

		if (hdat_file.get() == NULL)
		{
			std::string errmsg;
			int64 err = os_last_error(errmsg);
			VSSLog("Cannot open file hash data file for change block tracking. " + errmsg + " (code: " + convert(err) + ")", LL_ERROR);
			return false;
		}

		hdat_file->Resize(bitmap_data->BitmapSize * 8 * (sizeof(_u16) + chunkhash_single_size));

		VSSLog("Zeroing file hash data of volume " + volume + "...", LL_DEBUG);

		char zero_chunk[sizeof(_u16) + chunkhash_single_size] = {};

		DWORD curr_byte = 0;
		bool last_bit_set = false;
		bool last_zeroed = false;
		for (DWORD i = 0; i < bitmap_data->BitmapSize; i += bitmap_data->SectorSize)
		{
			for (DWORD j = i + URBT_MAGIC_SIZE; j < i + bitmap_data->SectorSize; ++j)
			{
				BYTE ch = bitmap_data->Bitmap[j];

				if (ch == 0)
				{
					if (last_bit_set)
					{
						if (hdat_file->Write(((int64)curr_byte * 8) * sizeof(zero_chunk), zero_chunk, sizeof(zero_chunk)) != sizeof(zero_chunk))
						{
							std::string errmsg;
							int64 err = os_last_error(errmsg);
							VSSLog("Errro zeroing file hash data. " + errmsg + " (code: " + convert(err) + ") -1", LL_ERROR);
							return false;
						}
					}

					last_bit_set = false;
					last_zeroed = false;

					++curr_byte;
					continue;
				}

				for (DWORD bit = 0; bit < 8; ++bit)
				{
					bool has_bit = (ch & (1 << bit)) > 0;
					if (has_bit
						|| last_bit_set)
					{
						if (!last_bit_set
							&& !last_zeroed)
						{
							int64 last_pos = (int64)curr_byte * 8 + bit;
							if (last_pos > 0)
							{
								--last_pos;
								if (hdat_file->Write(last_pos * sizeof(zero_chunk), zero_chunk, sizeof(zero_chunk)) != sizeof(zero_chunk))
								{
									std::string errmsg;
									int64 err = os_last_error(errmsg);
									VSSLog("Errro zeroing file hash data. " + errmsg + " (code: " + convert(err) + ") -2", LL_ERROR);
									return false;
								}
							}
						}

						if (hdat_file->Write(((int64)curr_byte * 8 + bit) * sizeof(zero_chunk), zero_chunk, sizeof(zero_chunk)) != sizeof(zero_chunk))
						{
							std::string errmsg;
							int64 err = os_last_error(errmsg);
							VSSLog("Errro zeroing file hash data. " + errmsg + " (code: " + convert(err) + ")", LL_ERROR);
							return false;
						}
						last_zeroed = true;
					}
					else
					{
						last_zeroed = false;
					}
					last_bit_set = has_bit;
				}
				++curr_byte;
			}
		}

		hdat_file->Sync();

		Server->deleteFile("urbackup\\hdat_file_" + conv_filename(strlower(volume)) + ".cbt");
	} //for_image_backup

#ifndef _DEBUG
	b = DeviceIoControl(hVolume, IOCTL_URBCT_RESET_FINISH, NULL, 0, NULL, 0, &bytesReturned, NULL);
#endif

	if (!b)
	{
		std::string errmsg;
		int64 err = os_last_error(errmsg);
		VSSLog("Finishing change block tracking reset for volume " + volume + " failed: " + errmsg + " (code: " + convert(err) + ")", LL_DEBUG);
		return false;
	}

	return true;
#else
	return false;
#endif
}

bool IndexThread::disableCbt(std::string volume)
{
#ifdef _WIN32
	if (!normalizeVolume(volume))
	{
		return true;
	}

	Server->Log("Disabling CBT on volume \"" + volume + "\"", LL_DEBUG);

	GUID rnd = randomGuid();
	std::string rndfn = "urbackup\\hdat_file_" + conv_filename(volume) + "_" + guidToString(rnd) + ".dat";
	if (os_rename_file("urbackup\\hdat_file_" + conv_filename(volume) + ".dat", rndfn))
	{
		Server->deleteFile(rndfn);
	}
	Server->deleteFile(ImageThread::hdatFn(volume));

	HANDLE hVolume = CreateFileA(("\\\\.\\" + volume).c_str(), GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);

	if (hVolume != INVALID_HANDLE_VALUE)
	{
		ScopedCloseWindowsHandle hclose(hVolume);
		DWORD bytesReturned;
		DeviceIoControl(hVolume, IOCTL_URBCT_MARK_ALL, NULL, 0, NULL, 0, &bytesReturned, NULL);
	}

	return !FileExists("urbackup\\hdat_file_" + conv_filename(volume) + ".dat")
		&& !FileExists(ImageThread::hdatFn(volume));
#else
	return true;
#endif
}

void IndexThread::enableCbtVol(std::string volume, bool install)
{
#ifdef _WIN32
	if (!normalizeVolume(volume))
	{
		return;
	}

	std::string allowed_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz:";
	
	for (size_t i = 0; i < volume.size(); ++i)
	{
		if (allowed_chars.find(volume[i]) == std::string::npos)
		{
			return;
		}
	}

	if (volume.size() == 1)
	{
		volume += ":";
	}

	if (install)
	{
		std::string crash_persistent = crashPersistentCbtIsEnabled(std::string(), volume) ? "crash-persistent" : "not-crash-persistent";

		system(("urbctctl.exe install " + volume + " " + crash_persistent).c_str());
	}
	else
	{
		system(("urbctctl.exe uninstall " + volume).c_str());
	}
#endif
}

void IndexThread::updateCbt()
{
#ifdef _WIN32
	if (os_get_file_type("urbctctl.exe")==0)
	{
		return;
	}

	std::set<std::string> vols;

	std::string settings_fn = "urbackup/data/settings.cfg";
	std::auto_ptr<ISettingsReader> curr_settings(Server->createFileSettingsReader(settings_fn));
	if (curr_settings.get() != NULL)
	{
		std::string volumes;

		if (curr_settings->getValue("image_letters", &volumes)
			|| curr_settings->getValue("image_letters_def", &volumes))
		{
			if (strlower(volumes) == "all")
			{
				volumes = get_all_volumes_list(false, volumes_cache);
			}
			else if (strlower(volumes) == "all_nonusb")
			{
				volumes = get_all_volumes_list(true, volumes_cache);
			}
			
			std::vector<std::string> ret;
			Tokenize(volumes, ret, ";,");
			for (size_t i = 0; i<ret.size(); ++i)
			{
				std::string cvol = strlower(trim(ret[i]));

				if (vols.find(cvol) == vols.end())
				{
					enableCbtVol(cvol, cbtIsEnabled(std::string(), cvol));
					vols.insert(cvol);
				}
			}
		}
	}

	readBackupDirs();

	for (size_t i = 0; i < backup_dirs.size(); ++i)
	{
		std::string cvol = strlower(trim(getVolPath(backup_dirs[i].path)));

		if (!cvol.empty()
			&& vols.find(cvol) == vols.end() )
		{
			enableCbtVol(cvol, cbtIsEnabled(std::string(), cvol));
			vols.insert(cvol);
		}
	}
#endif
}

void IndexThread::createMd5sumsFile(const std::string & path, std::string vol)
{
	normalizeVolume(vol);

	std::string fn = "md5sums-"+conv_filename(vol)+"-"+db->Read("SELECT strftime('%Y-%m-%d %H-%M', 'now', 'localtime') AS fn")[0]["fn"]+".txt";
	std::auto_ptr<IFile> output_f(Server->openFile(fn, MODE_WRITE));

	if (output_f.get() == NULL)
	{
		Server->Log("Error opening md5sums file. " + os_last_error_str(), LL_ERROR);
	}

	createMd5sumsFile(path, std::string(), output_f.get());
}

void IndexThread::createMd5sumsFile(const std::string & path, const std::string& md5sums_path, IFile * output_f)
{
	std::vector<SFile> files = getFiles(os_file_prefix(path));
	
	for (size_t i = 0; i < files.size(); ++i)
	{
		if (files[i].isdir
			&& !files[i].issym)
		{
			createMd5sumsFile(path + os_file_sep() + files[i].name,
				md5sums_path.empty() ? files[i].name : (md5sums_path + "/" + files[i].name),
				output_f);
		}
		else if (!files[i].isspecialf)
		{
			std::auto_ptr<IFile> f(Server->openFile(os_file_prefix(path + os_file_sep() + files[i].name), MODE_READ_SEQUENTIAL_BACKUP));

			if (f.get() == NULL)
			{
				Server->Log("Error opening file \"" + path + os_file_sep() + files[i].name + "\" for creating md5sums. " + os_last_error_str(), LL_ERROR);
			}
			else
			{
				MD5 md5;
				std::vector<char> buf;
				buf.resize(32768);

				bool has_read_error = false;
				_u32 rc;
				while ((rc=f->Read(buf.data(), static_cast<_u32>(buf.size()), &has_read_error)) > 0)
				{
					md5.update(reinterpret_cast<unsigned char*>(buf.data()), static_cast<_u32>(rc));
				}

				if (has_read_error)
				{
					Server->Log("Error while reading from file \"" + path + os_file_sep() + files[i].name + "\" for creating md5sums. " + os_last_error_str(), LL_ERROR);
				}

				md5.finalize();

				char* hd = md5.hex_digest();
				std::string hex_dig(hd);
				delete[] hd;

				output_f->Write(hex_dig + "  " + (md5sums_path.empty() ? files[i].name : (md5sums_path + "/" + files[i].name)) + "\n");
			}
		}
	}
}

void IndexThread::setFlags( unsigned int flags )
{
	calculate_filehashes_on_client = (flags & flag_calc_checksums)>0;
	end_to_end_file_backup_verification = (flags & flag_end_to_end_verification)>0;
	with_scripts = (flags & flag_with_scripts)>0;
	with_orig_path = (flags & flag_with_orig_path)>0;
	with_sequence = (flags & flag_with_sequence)>0;
	with_proper_symlinks = (flags & flag_with_proper_symlinks)>0;
}

bool IndexThread::getAbsSymlinkTarget( const std::string& symlink, const std::string& orig_path,
	std::string& target, std::string& output_target)
{
	if (target.empty())
	{
		if (!os_get_symlink_target(symlink, target))
		{
			if (!(index_flags & EBackupDirFlag_SymlinksOptional) && (index_flags & EBackupDirFlag_FollowSymlinks))
			{
				VSSLog("Error getting symlink target of symlink " + symlink + ". Not following symlink.", LL_WARNING);
			}
			return false;
		}

		if (!os_path_absolute(target))
		{
			target = orig_path + os_file_sep() + target;
		}

		target = os_get_final_path(target);
	}

	std::string lower_target;
#ifdef _WIN32
	lower_target = strlower(target);
#else
	lower_target = target;
#endif

	for(size_t i=0;i<backup_dirs.size();++i)
	{
		if(backup_dirs[i].group!=index_group)
			continue;

		if(backup_dirs[i].symlinked
			&& !(index_flags & EBackupDirFlag_FollowSymlinks))
			continue;

		std::string bpath = addDirectorySeparatorAtEnd(backup_dirs[i].path);
		std::string bpath_wo_slash;
		
		#ifndef _WIN32
		bpath_wo_slash = removeDirectorySeparatorAtEnd(bpath);
		if(bpath.empty())
		{
			bpath="/";
		}
		#else
		bpath = strlower(bpath);
		bpath_wo_slash = removeDirectorySeparatorAtEnd(bpath);
		#endif
		
		if(removeDirectorySeparatorAtEnd(lower_target) == bpath_wo_slash
			|| next(lower_target, 0, bpath))
		{
			if (target.size() > bpath.size())
			{
				output_target = target.substr(bpath.size());
			}
			output_target = backup_dirs[i].tname + (output_target.empty() ? "" : (os_file_sep() + removeDirectorySeparatorAtEnd(output_target)));

			if(backup_dirs[i].symlinked && !backup_dirs[i].symlinked_confirmed)
			{
				VSSLog("Following symbolic link at \""+symlink+"\" to \""+ target +"\" confirms symlink backup target \""+backup_dirs[i].tname+"\" to \""+backup_dirs[i].path+"\"", LL_INFO);
				backup_dirs[i].symlinked_confirmed=true;
			}

			return true;
		}
	}

	if(index_flags & EBackupDirFlag_FollowSymlinks)
	{
		VSSLog("Following symbolic link at \""+symlink+"\" to new symlink backup target at \""+target+"\"", LL_INFO);
		addSymlinkBackupDir(target, output_target);
		return true;
	}
	else
	{
		Server->Log("Not following symlink "+symlink+" because of configuration.", LL_DEBUG);
		return false;
	}
}

void IndexThread::addSymlinkBackupDir( const std::string& target, std::string& output_target )
{
	std::string name=".symlink_"+ExtractFileName(target);

	//Make sure it is unique
	if(backupNameInUse(name))
	{
		int n=1;
		std::string add="_"+convert(n);
		while(backupNameInUse(name+add))
		{
			add="_"+convert(++n);
		}
		name+=add;
	}

	output_target = name;

	SBackupDir backup_dir;

	cd->addBackupDir(name, target, index_server_default ? 1: 0, index_flags, index_group, 1);

	backup_dir.id=static_cast<int>(db->getLastInsertID());

#ifdef _WIN32
	if(dwt!=NULL)
	{
		std::string msg="A"+target;
		dwt->getPipe()->Write(msg);
    }
#endif

	
	backup_dir.group=index_group;
	backup_dir.flags=index_flags;
	backup_dir.path=target;
	backup_dir.tname=name;
	backup_dir.symlinked=true;
	backup_dir.symlinked_confirmed=true;
	backup_dir.server_default = index_server_default;

	backup_dirs.push_back(backup_dir);

	shareDir(std::string(), name, target);
}

bool IndexThread::backupNameInUse( const std::string& name )
{
	for(size_t i=0;i<backup_dirs.size();++i)
	{
		if(backup_dirs[i].tname==name)
		{
			return true;
		}
	}
	return false;
}

void IndexThread::removeUnconfirmedSymlinkDirs(size_t off)
{
	for(size_t i=off;i<backup_dirs.size();)
	{
		if(index_group == backup_dirs[i].group)
		{
			if(backup_dirs[i].symlinked
				&& !backup_dirs[i].symlinked_confirmed)
			{
				VSSLog("Not backing up unconfirmed symbolic link \"" + backup_dirs[i].tname + "\" to \"" + backup_dirs[i].path, LL_INFO);
#ifdef _WIN32
				if(dwt!=NULL)
				{
					std::string msg="D"+backup_dirs[i].path;
					dwt->getPipe()->Write(msg);
				}
#endif

				cd->delBackupDir(backup_dirs[i].id);

				removeDir(starttoken, backup_dirs[i].tname);
				removeDir(std::string(), backup_dirs[i].tname);

				if(filesrv!=NULL)
				{
					filesrv->removeDir(backup_dirs[i].tname, starttoken);
					filesrv->removeDir(backup_dirs[i].tname, std::string());
				}

				backup_dirs.erase(backup_dirs.begin() + i);

				continue;
			}
			else
			{
				break;
			}
		}
		++i;
	}
}

void IndexThread::filterEncryptedFiles(const std::string & dir, const std::string& orig_dir, std::vector<SFile>& files)
{
	bool has_encrypted = false;
	for (size_t i = 0; i < files.size(); ++i)
	{
		if (files[i].isencrypted)
		{
			has_encrypted = true;
		}
	}

	if (has_encrypted)
	{
		std::vector<SFile> new_files;

		for (size_t i = 0; i < files.size(); ++i)
		{
			if (files[i].isencrypted
				&& files[i].isdir)
			{
				bool has_error = false;
				getFiles(os_file_prefix(dir + os_file_sep() + files[i].name), &has_error);

				if (has_error)
				{
					VSSLog("Not backing up encrypted directory \"" + orig_dir + os_file_sep() + files[i].name + "\" (Cannot read directory contents: " + os_last_error_str() + "). See https://www.urbackup.org/faq.html#windows_efs", LL_WARNING);
				}
				else
				{
					new_files.push_back(files[i]);
				}
			}
			else if (files[i].isencrypted
				&& !files[i].isdir)
			{
#ifdef _WIN32
				HANDLE hFile = CreateFileW(Server->ConvertToWchar(os_file_prefix(dir + os_file_sep() + files[i].name)).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
					OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);

				if (hFile == INVALID_HANDLE_VALUE)
				{
					VSSLog("Not backing up encrypted file \"" + orig_dir + os_file_sep() + files[i].name + "\" (Cannot read file contents: " + os_last_error_str() + "). See https://www.urbackup.org/faq.html#windows_efs", LL_WARNING);
				}
				else
				{
					CloseHandle(hFile);

					new_files.push_back(files[i]);
				}
#else
				new_files.push_back(files[i]);
#endif
			}
			else
			{
				new_files.push_back(files[i]);
			}
		}

		files = new_files;
	}
}

std::vector<SFileAndHash> IndexThread::convertToFileAndHash( const std::string& orig_dir, const std::string& named_path, const std::vector<std::string>& exclude_dirs,
	const std::vector<SIndexInclude>& include_dirs, const std::vector<SFile> files, const std::string& fn_filter)
{
	std::vector<SFileAndHash> ret;
	if(fn_filter.empty())
	{
		ret.resize(files.size());
	}

	for(size_t i=0;i<files.size();++i)
	{
		SFileAndHash* curr;
		if(!fn_filter.empty())
		{
			if(files[i].name==fn_filter)
			{
				ret.resize(1);
				curr=&ret[0];
			}
			else
			{
				continue;
			}			
		}
		else
		{
			curr=&ret[i];
		}
		

		curr->isdir=files[i].isdir;
		curr->change_indicator = getChangeIndicator(files[i]);
		curr->name=files[i].name;
		curr->size=files[i].size;
		curr->issym=files[i].issym;
		curr->isspecialf=files[i].isspecialf;
		curr->nlinks = files[i].nlinks;

		if(curr->issym && with_proper_symlinks
			&& !skipFile(orig_dir + os_file_sep() + files[i].name, named_path + os_file_sep() + files[i].name,
				exclude_dirs, include_dirs) )
		{
			if(!getAbsSymlinkTarget(orig_dir+os_file_sep()+files[i].name, orig_dir, ret[i].symlink_target, ret[i].output_symlink_target))
			{
				if(!(index_flags & EBackupDirFlag_SymlinksOptional) && (index_flags & EBackupDirFlag_FollowSymlinks) )
				{
					VSSLog("Error getting symlink target of symlink "+orig_dir+os_file_sep()+files[i].name, LL_ERROR);
				}
			}
		}
	}
	return ret;
}

void IndexThread::handleSymlinks(const std::string& orig_dir, std::string named_path, const std::vector<std::string>& exclude_dirs,
	const std::vector<SIndexInclude>& include_dirs, std::vector<SFileAndHash>& files)
{
	for (size_t i = 0; i < files.size(); ++i)
	{
		if (files[i].issym)
		{
			if (skipFile(orig_dir + os_file_sep() + files[i].name, named_path + os_file_sep() + files[i].name,
				exclude_dirs, include_dirs))
			{
				continue;
			}

			if (!getAbsSymlinkTarget(orig_dir + os_file_sep() + files[i].name, orig_dir, files[i].symlink_target, files[i].output_symlink_target))
			{
				if (!(index_flags & EBackupDirFlag_SymlinksOptional) && (index_flags & EBackupDirFlag_FollowSymlinks))
				{
					VSSLog("Error getting symlink target of symlink " + orig_dir + os_file_sep() + files[i].name, LL_ERROR);
				}
			}
		}
	}
}

int64 IndexThread::randomChangeIndicator()
{
	int64 rnd = Server->getTimeSeconds() << 32 | Server->getRandomNumber();
	rnd &= ~change_indicator_all_bits;
	return rnd;
}

int64 IndexThread::getChangeIndicator(const std::string& path)
{
	SFile file = getFileMetadata(os_file_prefix(path));
	return getChangeIndicator(file);
}

int64 IndexThread::getChangeIndicator(const SFile & file)
{
	int64 change_indicator;
	if (file.usn == 0)
	{
		change_indicator = file.last_modified;
	}
	else
	{
		change_indicator = file.usn;
	}

	if (file.issym)
	{
		change_indicator |= (change_indicator_symlink_bit | change_indicator_special_bit);
	}
	else if (file.isspecialf)
	{
		change_indicator |= change_indicator_special_bit;
	}
	else
	{
		change_indicator &= ~change_indicator_all_bits;
	}
	return change_indicator;
}

#ifndef _WIN32
bool IndexThread::start_shadowcopy_lin( SCDirs * dir, std::string &wpath, bool for_imagebackup, bool * &onlyref, bool* not_configured)
{
	std::string scriptname;
	if(dir->fileserv)
	{
		scriptname="create_filesystem_snapshot";
	}
	else
	{
		scriptname="create_volume_snapshot";
	}

	std::string scriptlocation = get_snapshot_script_location(scriptname);

	if (scriptlocation.empty())
	{
		if (not_configured != NULL)
		{
			*not_configured = true;
		}
		return false;
	}

	GUID ssetid = randomGuid();

	std::string loglines;
	int rc = os_popen(scriptlocation +" "+guidToString(ssetid)+" "+escapeDirParam(dir->ref->target)+" "+
		escapeDirParam(dir->dir)+" "+escapeDirParam(dir->orig_target)
		+ (index_clientsubname.empty()?"":(" " + escapeDirParam(index_clientsubname)))+" 2>&1", loglines);

	if(rc!=0)
	{
		VSSLog("Creating snapshot of \""+dir->orig_target+"\" failed", LL_ERROR);
		VSSLogLines(loglines, LL_ERROR);
		return false;
	}

	std::vector<std::string> lines;
	TokenizeMail(loglines, lines, "\n");
	std::string snapshot_target;
	for(size_t i=0;i<lines.size();++i)
	{
		std::string line = trim(lines[i]);

		if(next(line, 0, "SNAPSHOT="))
		{
			snapshot_target = line.substr(9);
		}
		else
		{
			VSSLog(line, LL_INFO);
		}
	}

	if(snapshot_target.empty())
	{
		VSSLog("Could not find snapshot target. Please include a snapshot target output in the script (e.g. echo SNAPSHOT=/mnt/snap/xxxx)", LL_ERROR);
		return false;
	}

	dir->target.erase(0,wpath.size());

	if(dir->target.empty() || dir->target[0]!='/')
	{
		dir->target = "/" + dir->target;
	}

	dir->ref->volpath=(snapshot_target);
	dir->starttime=Server->getTimeSeconds();
	dir->target= removeDirectorySeparatorAtEnd(dir->ref->volpath+ (dir->target.empty()? "" : dir->target));
	if(dir->fileserv)
	{
		shareDir(starttoken, dir->dir, dir->target);
	}

	SShadowCopy tsc;
	tsc.vssid=ssetid;
	tsc.ssetid=ssetid;
	tsc.target=dir->orig_target;
	tsc.path=dir->ref->volpath;
	tsc.orig_target=dir->orig_target;
	tsc.filesrv=dir->fileserv;
	tsc.vol=wpath;
	tsc.tname=dir->dir;
	tsc.starttoken=starttoken;
	tsc.clientsubname = index_clientsubname;
	if(for_imagebackup)
	{
		tsc.refs=1;
	}
	dir->ref->save_id=cd->addShadowcopy(tsc);
	dir->ref->ok=true;
	dir->ref->ssetid = ssetid;

	VSSLog("Shadowcopy path: "+tsc.path, LL_DEBUG);

	if(onlyref!=NULL)
	{
		*onlyref=false;
	}
	return true;
}

std::string IndexThread::get_snapshot_script_location(const std::string & name)
{
	std::string conffile = SYSCONFDIR "/urbackup/snapshot.cfg";
	if (!FileExists(conffile))
	{
		return std::string();
	}
	std::auto_ptr<ISettingsReader> settings(Server->createFileSettingsReader(conffile));

	std::string ret;
	if (!index_clientsubname.empty()
		&& settings->getValue(conv_filename(index_clientsubname) + "_" + name, &ret))
	{
		return trim(ret);
	}

	if (settings->getValue(name, &ret))
	{
		return trim(ret);
	}

	return std::string();
}

bool IndexThread::get_volumes_mounted_locally()
{
	std::string ret = strlower(get_snapshot_script_location("volumes_mounted_locally"));

	return ret != "0" && ret != "false" && ret != "no";
}


#endif //!_WIN32

std::string IndexThread::escapeDirParam( const std::string& dir )
{
	return "\""+greplace("\"", "\\\"", dir)+"\"";
}

int IndexThread::getShadowId(const std::string & volume, IFile* hdat_img)
{
	IScopedLock lock(cbt_shadow_id_mutex);

	std::map<std::string, int>::iterator it = cbt_shadow_ids.find(strlower(volume));

	if (it != cbt_shadow_ids.end())
	{
		return it->second;
	}
	else
	{
		if (hdat_img != NULL)
		{
			int shadow_id;
			if (hdat_img->Read(0, reinterpret_cast<char*>(&shadow_id), sizeof(shadow_id)) == sizeof(shadow_id))
			{
				cbt_shadow_ids[strlower(volume)] = shadow_id;
				return shadow_id;
			}
		}

		return -1;
	}
}

#ifndef _WIN32
std::string IndexThread::lookup_shadowcopy(int sid)
{
	return std::string();
}

void IndexThread::clearContext(SShadowCopyContext& context)
{
}
#endif

void IndexThread::addScRefs(VSS_ID ssetid, std::vector<SCRef*>& out)
{
	for (size_t i = 0; i < sc_refs.size(); ++i)
	{
		if (sc_refs[i]->ssetid == ssetid)
		{
			out.push_back(sc_refs[i]);
		}
	}
}

void IndexThread::openCbtHdatFile(SCRef* ref, const std::string& sharename, const std::string & volume)
{
	if (ref!=NULL
		&& ref->cbt)
	{
		std::string vol = volume;
		normalizeVolume(vol);
		vol = strlower(vol);

		index_hdat_file.reset(Server->openFile("urbackup/hdat_file_" + conv_filename(vol) + ".dat", MODE_RW_CREATE_DELETE));
		index_hdat_fs_block_size = -1;

#ifdef _WIN32
		DWORD sectors_per_cluster;
		DWORD bytes_per_sector;
		BOOL b = GetDiskFreeSpaceW((Server->ConvertToWchar(vol) + L"\\").c_str(),
			&sectors_per_cluster, &bytes_per_sector, NULL, NULL);
		if (!b)
		{
			VSSLog("Error in GetDiskFreeSpaceW. " + os_last_error_str(), LL_ERROR);
		}
		else
		{
			index_hdat_fs_block_size = bytes_per_sector * sectors_per_cluster;
		}
#endif
		size_t* seq_id = &index_hdat_sequence_ids[strlower(vol)];
		if (index_hdat_file.get()
			&& filesrv != NULL
			&& index_hdat_fs_block_size>0)
		{
			IFsFile* f = Server->openFile(index_hdat_file->getFilename(), MODE_RW_DELETE);
			if (f != NULL)
			{
				filesrv->setCbtHashFile(starttoken + "|" + sharename, std::string(),
					IFileServ::CbtHashFileInfo(f, index_hdat_fs_block_size, seq_id, *seq_id));
			}
		}

		client_hash.reset(new ClientHash(index_hdat_file.get(), false, index_hdat_fs_block_size,
			seq_id, *seq_id));

		if (phash_queue != NULL)
		{
			IFsFile* f = Server->openFile(index_hdat_file->getFilename(), MODE_RW_DELETE);
			if (f != NULL)
			{
				CWData data;
				data.addChar(ID_CBT_DATA);
				data.addVoidPtr(f);
				data.addVarInt(index_hdat_fs_block_size);
				data.addVoidPtr(seq_id);
				data.addVarInt(*seq_id);

				if (!addToPhashQueue(data))
				{
					Server->destroy(f);
				}
			}
			else
			{
				CWData data;
				data.addChar(ID_INIT_HASH);
				addToPhashQueue(data);
			}
		}
	}
	else
	{
		index_hdat_file.reset();
		client_hash.reset(new ClientHash(NULL, false, 0, NULL, 0));

		if (phash_queue != NULL)
		{
			CWData data;
			data.addChar(ID_INIT_HASH);
			addToPhashQueue(data);
		}

		std::string vol = volume;
		normalizeVolume(vol);
		vol = strlower(vol);

		std::map<std::string, size_t>::iterator it = index_hdat_sequence_ids.find(strlower(vol));
		if (it != index_hdat_sequence_ids.end())
		{
			++it->second;
		}
	}
}

void IndexThread::readSnapshotGroups()
{
	std::string settings_fn = "urbackup/data/settings.cfg";
	if (!index_clientsubname.empty())
	{
		settings_fn = "urbackup/data/settings_" + conv_filename(index_clientsubname) + ".cfg";
	}

	image_snapshot_groups.clear();
	file_snapshot_groups.clear();

	std::auto_ptr<ISettingsReader> curr_settings(Server->createFileSettingsReader(settings_fn));
	if (curr_settings.get() != NULL)
	{
		readSnapshotGroup(curr_settings.get(), "image_snapshot_groups", image_snapshot_groups);
		readSnapshotGroup(curr_settings.get(), "file_snapshot_groups", file_snapshot_groups);
	}
}

void IndexThread::readSnapshotGroup(ISettingsReader *curr_settings, const std::string & settings_name, std::vector<std::vector<std::string> >& groups)
{
	bool has_volumes = false;
	std::string volumes_str;
	std::vector<std::string> volumes;
#ifdef _WIN32
	if (settings_name == "image_snapshot_groups")
	{
		if (curr_settings->getValue("image_letters", &volumes_str)
			|| curr_settings->getValue("image_letters_def", &volumes_str))
		{
			if (strlower(volumes_str) == "all")
			{
				volumes_str = get_all_volumes_list(false, volumes_cache);
			}
			else if (strlower(volumes_str) == "all_nonusb")
			{
				volumes_str = get_all_volumes_list(true, volumes_cache);
			}

			Tokenize(volumes_str, volumes, ";,");
			for (size_t i = 0; i < volumes.size(); ++i)
			{
				normalizeVolume(volumes[i]);
				volumes[i] = strlower(volumes[i]);
			}
			has_volumes = true;
		}
	}
#endif

	std::string val;
	if (curr_settings->getValue(settings_name, &val) || curr_settings->getValue(settings_name + "_def", &val))
	{
		if (trim(strlower(val)) == "all")
		{
			if (settings_name == "image_snapshot_groups")
			{
				groups.push_back(volumes);
				return;
			}
			else if (settings_name=="file_snapshot_groups")
			{
				std::vector<std::string> groups_mem;
				for (size_t i = 0; i < backup_dirs.size(); ++i)
				{
					if (backup_dirs[i].group == index_group)
					{
						std::string vol = backup_dirs[i].path;
						normalizeVolume(vol);

#ifdef _WIN32
						vol = strlower(vol);
#endif

						if (std::find(groups_mem.begin(), groups_mem.end(), vol) == groups_mem.end())
						{
							groups_mem.push_back(vol);
						}
					}
				}

				groups.push_back(groups_mem);
				return;
			}
		}

		std::vector<std::string> groups_str;
		TokenizeMail(val, groups_str, "|");

		for (size_t i = 0; i < groups_str.size(); ++i)
		{
			std::vector<std::string> groups_mem;
			TokenizeMail(groups_str[i], groups_mem, ";,");

			if (!groups_mem.empty())
			{
				for (size_t j = 0; j < groups_mem.size();)
				{
					normalizeVolume(groups_mem[j]);
#ifdef _WIN32
					groups_mem[j] = strlower(groups_mem[j]);
#endif
					if (has_volumes
						&& std::find(volumes.begin(), volumes.end(), groups_mem[j])
							== volumes.end())
					{
						groups_mem.erase(groups_mem.begin() + j);
						continue;
					}

					++j;
				}

				groups.push_back(groups_mem);
			}
		}
	}
}

std::vector<std::string> IndexThread::getSnapshotGroup(std::string volume, bool for_image)
{
	if (!normalizeVolume(volume))
	{
		return std::vector<std::string>();
	}

#ifdef _WIN32
	volume = strlower(volume);
#endif

	std::vector< std::vector<std::string> >& groups = for_image ? image_snapshot_groups : file_snapshot_groups;

	for (size_t i = 0; i < groups.size(); ++i)
	{
		if (std::find(groups[i].begin(), groups[i].end(), volume) != groups[i].end())
		{


			return groups[i];
		}
	}

	return std::vector<std::string>();
}

std::string IndexThread::otherVolumeInfo(SCDirs * dir, bool onlyref)
{
	if (onlyref
		|| dir->ref==NULL)
	{
		return std::string();
	}

	std::string other_vols;
	for (size_t i = 0; i < sc_refs.size(); ++i)
	{
		if (sc_refs[i]->volid != dir->ref->volid
			&& sc_refs[i]->ssetid == dir->ref->ssetid)
		{
			other_vols += "&vol_" + convert(i) + "=" + EscapeParamString(sc_refs[i]->target);
			other_vols += "&id_" + convert(i) + "=" + convert(sc_refs[i]->save_id);
		}
	}

	if (!other_vols.empty())
	{
		other_vols[0] = '|';
	}

	return other_vols;
}

void IndexThread::postSnapshotProcessing(SCDirs* scd, bool full_backup)
{
	if (!full_backup)
	{
#ifdef _WIN32
		DirectoryWatcherThread::update_and_wait(open_files);
#endif
		std::sort(open_files.begin(), open_files.end());
	}

	if (scd!=NULL 
		&& scd->ref != NULL)
	{
		for (size_t k = 0; k < sc_refs.size(); ++k)
		{
			if (sc_refs[k]->ssetid == scd->ref->ssetid)
			{
				if (sc_refs[k]->cbt)
				{
					sc_refs[k]->cbt = finishCbt(sc_refs[k]->target, -1, sc_refs[k]->volpath, false);
				}

				postSnapshotProcessing(sc_refs[k], full_backup);
			}
		}
	}	
}

void IndexThread::postSnapshotProcessing(SCRef * ref, bool full_backup)
{
	if (full_backup)
	{
		return;
	}

#ifdef _WIN32
	std::string volpath = removeDirectorySeparatorAtEnd(getVolPath(ref->target));

	volpath = strlower(volpath);
#else
	std::string volpath = ref->target;
#endif

	if (volpath.empty())
	{
		VSSLog("Error getting volume path for " + ref->target, LL_WARNING);
	}

	std::vector<int> db_tgroup;
	for (size_t i = 0; i < backup_dirs.size(); ++i)
	{
		std::string path = backup_dirs[i].path;
#ifdef _WIN32
		path = strlower(path);
#endif
		if (backup_dirs[i].group == index_group
			&& next(path, 0, volpath))
		{
			int tgroup = (backup_dirs[i].flags & EBackupDirFlag_ShareHashes) ? 0 : (backup_dirs[i].group + 1);;
			if (std::find(db_tgroup.begin(), db_tgroup.end(), tgroup) == db_tgroup.end())
			{
				db_tgroup.push_back(tgroup);
			}
		}
	}

	std::vector<std::string> acd = cd->getChangedDirs(volpath, false);
	for (size_t j = 0; j < acd.size(); ++j)
	{
		if (!std::binary_search(changed_dirs.begin(), changed_dirs.end(), acd[j]))
		{
			changed_dirs.push_back(acd[j]);
		}
	}
	std::sort(changed_dirs.begin(), changed_dirs.end());

	VSSLog("Removing deleted directories from index for \"" + volpath + "\"...", LL_DEBUG);
	std::vector<std::string> deldirs = cd->getDelDirs(volpath, false);
	DBScopedWriteTransaction write_transaction(db);
	for (size_t j = 0; j < deldirs.size(); ++j)
	{
		for (size_t i = 0; i < db_tgroup.size(); ++i)
		{
			cd->removeDeletedDir(deldirs[j], db_tgroup[i]);
		}
	}

	VSSLog("Scanning for changed hard links on volume of \"" + ref->target + "\"...", LL_INFO);
	handleHardLinks(ref->target, ref->volpath, volpath);
}

void IndexThread::initParallelHashing(const std::string & async_ticket)
{
	phash_queue = Server->openTemporaryFile();
	phash_queue_write_pos = 0;
	os_create_dir(Server->getServerWorkingDir() + "urbackup" + os_file_sep() + "phash");
	filesrv->shareDir("phash_{9c28ff72-5a74-487b-b5e1-8f1c96cd0cf4}", Server->getServerWorkingDir() + "/urbackup/phash", std::string(), true);
	ParallelHash* phash = new ParallelHash(phash_queue, sha_version);
	filesrv->registerScriptPipeFile("phash_"+bytesToHex(async_ticket), phash);
}

bool IndexThread::addToPhashQueue(CWData & data)
{
	_u32 msgsize = static_cast<_u32>(data.getDataSize());
	phash_queue_buffer.insert(phash_queue_buffer.end(), reinterpret_cast<char*>(&msgsize), reinterpret_cast<char*>(&msgsize) + sizeof(msgsize));
	phash_queue_buffer.insert(phash_queue_buffer.end(), data.getDataPtr(), data.getDataPtr() + data.getDataSize());
	return true;
}

bool IndexThread::commitPhashQueue()
{
	if (phash_queue == NULL)
		return true;

	bool ret = phash_queue->Write(phash_queue_write_pos, phash_queue_buffer.data(), static_cast<_u32>(phash_queue_buffer.size())) == phash_queue_buffer.size();
	if (ret)
	{
		phash_queue_write_pos += phash_queue_buffer.size();
	}
	phash_queue_buffer.clear();
	return ret;
}
