/*************************************************************************
*    UrBackup - Client/Server backup system
*    Copyright (C) 2011-2015 Martin Raiber
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

#include "server_hash_existing.h"
#include "server_prepare_hash.h"
#include "../Interface/Server.h"
#include "server_log.h"

ServerHashExisting::ServerHashExisting( int clientid, logid_t logid, IncrFileBackup* incr_backup )
	: has_error(false), clientid(clientid), incr_backup(incr_backup), logid(logid)
{
	mutex = Server->createMutex();
	cond = Server->createCondition();
}


ServerHashExisting::~ServerHashExisting()
{
	Server->destroy(mutex);
	Server->destroy(cond);
}


void ServerHashExisting::operator()()
{
	while(true)
	{
		SHashItem item;
		{
			IScopedLock lock(mutex);
			while(queue.empty())
			{
				cond->wait(&lock);
			}

			item = queue.front();
			queue.pop_front();
		}

		if(item.do_stop)
		{
			return;
		}

		IFile* f = Server->openFile(item.fullpath, MODE_READ);

		if(f==NULL)
		{
			ServerLogger::Log(logid, "Error opening file \""+item.hashpath+"\" for hashing", LL_WARNING);
			has_error = true;
		}
		else
		{
			ObjectScope destroy_f(f);

			int64 filesize = f->Size();
			std::string sha2 = BackupServerPrepareHash::hash_sha(f);

			incr_backup->addExistingHash(item.fullpath, item.hashpath, sha2, filesize, -1);
		}
	}
}

void ServerHashExisting::queueStop()
{
	SHashItem item;
	item.do_stop = true;
	IScopedLock lock(mutex);
	queue.push_back(item);
	cond->notify_one();
}

void ServerHashExisting::queueFile( const std::string& fullpath, const std::string& hashpath )
{
	SHashItem item;
	item.fullpath = fullpath;
	item.hashpath = hashpath;

	IScopedLock lock(mutex);
	queue.push_back(item);
	cond->notify_one();
}


