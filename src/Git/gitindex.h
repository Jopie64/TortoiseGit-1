#include "GitHash.h"
#include "gitdll.h"
#include "gitstatus.h"
#include "SharedMutex.h"

/* Copy from Git cache.h*/
#define FLEX_ARRAY 4

#pragma pack(push)
#pragma pack(1)
//#pragma pack(show) 
#define CACHE_SIGNATURE 0x44495243	/* "DIRC" */
struct cache_header {
	unsigned int hdr_signature;
	unsigned int hdr_version;
	unsigned int hdr_entries;
};

/*
 * The "cache_time" is just the low 32 bits of the
 * time. It doesn't matter if it overflows - we only
 * check it for equality in the 32 bits we save.
 */
struct cache_time {
	UINT32 sec;
	UINT32 nsec;
};

/*
 * dev/ino/uid/gid/size are also just tracked to the low 32 bits
 * Again - this is just a (very strong in practice) heuristic that
 * the inode hasn't changed.
 *
 * We save the fields in big-endian order to allow using the
 * index file over NFS transparently.
 */
struct ondisk_cache_entry {
	struct cache_time ctime;
	struct cache_time mtime;
	UINT32 dev;
	UINT32 ino;
	UINT32 mode;
	UINT32 uid;
	UINT32 gid;
	UINT32 size;
	BYTE sha1[20];
	UINT16 flags;
	char name[FLEX_ARRAY]; /* more */
};

/*
 * This struct is used when CE_EXTENDED bit is 1
 * The struct must match ondisk_cache_entry exactly from
 * ctime till flags
 */
struct ondisk_cache_entry_extended {
	struct cache_time ctime;
	struct cache_time mtime;
	UINT32 dev;
	UINT32 ino;
	UINT32 mode;
	UINT32 uid;
	UINT32 gid;
	UINT32 size;
	BYTE sha1[20];
	UINT16 flags;
	UINT16 flags2;
	char name[FLEX_ARRAY]; /* more */
};

#pragma pack(pop)

#define CE_NAMEMASK  (0x0fff)
#define CE_STAGEMASK (0x3000)
#define CE_EXTENDED  (0x4000)
#define CE_VALID     (0x8000)
#define CE_STAGESHIFT 12
/*
 * Range 0xFFFF0000 in ce_flags is divided into
 * two parts: in-memory flags and on-disk ones.
 * Flags in CE_EXTENDED_FLAGS will get saved on-disk
 * if you want to save a new flag, add it in
 * CE_EXTENDED_FLAGS
 *
 * In-memory only flags
 */
#define CE_UPDATE    (0x10000)
#define CE_REMOVE    (0x20000)
#define CE_UPTODATE  (0x40000)
#define CE_ADDED     (0x80000)

#define CE_HASHED    (0x100000)
#define CE_UNHASHED  (0x200000)

/*
 * Extended on-disk flags
 */
#define CE_INTENT_TO_ADD 0x20000000
/* CE_EXTENDED2 is for future extension */
#define CE_EXTENDED2 0x80000000

#define CE_EXTENDED_FLAGS (CE_INTENT_TO_ADD)

/*
 * Safeguard to avoid saving wrong flags:
 *  - CE_EXTENDED2 won't get saved until its semantic is known
 *  - Bits in 0x0000FFFF have been saved in ce_flags already
 *  - Bits in 0x003F0000 are currently in-memory flags
 */
#if CE_EXTENDED_FLAGS & 0x803FFFFF
#error "CE_EXTENDED_FLAGS out of range"
#endif

/*
 * Copy the sha1 and stat state of a cache entry from one to
 * another. But we never change the name, or the hash state!
 */
#define CE_STATE_MASK (CE_HASHED | CE_UNHASHED)

template<class T> 
T Big2lit(T data)
{
	T ret;
	BYTE *p1=(BYTE*)&data;
	BYTE *p2=(BYTE*)&ret;
	for(int i=0;i<sizeof(T);i++)
	{
		p2[sizeof(T)-i-1] = p1[i];
	}
	return ret;
}

template<class T>
static inline size_t ce_namelen(T *ce)
{
	size_t len = Big2lit(ce->flags) & CE_NAMEMASK;
	if (len < CE_NAMEMASK)
		return len;
	return strlen(ce->name + CE_NAMEMASK) + CE_NAMEMASK;
}

#define flexible_size(STRUCT,len) ((offsetof(STRUCT,name) + (len) + 8) & ~7)

//#define ondisk_cache_entry_size(len) flexible_size(ondisk_cache_entry,len)
//#define ondisk_cache_entry_extended_size(len) flexible_size(ondisk_cache_entry_extended,len)

//#define ondisk_ce_size(ce) (((ce)->flags & CE_EXTENDED) ? \
//			    ondisk_cache_entry_extended_size(ce_namelen(ce)) : \
//			    ondisk_cache_entry_size(ce_namelen(ce)))

template<class T>
static inline size_t ondisk_ce_size(T *ce)
{
	return flexible_size(T,ce_namelen(ce));
}

class CGitIndex
{
public:
	CString    m_FileName;
	__time64_t	m_ModifyTime;
	int			m_Flags;
	//int		 m_Status;
	CGitHash	m_IndexHash;
	
	int FillData(ondisk_cache_entry* entry);
	int FillData(ondisk_cache_entry_extended* entry);
	int Print();

};

class CAutoReadLock
{
	SharedMutex *m_Lock;
public:
	CAutoReadLock(SharedMutex * lock)
	{
		m_Lock = lock;
		lock->AcquireShared();
	}
	~CAutoReadLock()
	{
		m_Lock->ReleaseShared();
	}
};

class CAutoWriteLock
{
	SharedMutex *m_Lock;
public:
	CAutoWriteLock(SharedMutex * lock)
	{
		m_Lock = lock;
		lock->AcquireExclusive();
	}
	~CAutoWriteLock()
	{
		m_Lock->ReleaseExclusive();
	}
};

class CGitIndexList:public std::vector<CGitIndex>
{
protected:
	
public:
	std::map<CString,int> m_Map;
	__time64_t  m_LastModifyTime;
	
	SharedMutex  m_SharedMutex;

	CGitIndexList();
	int ReadIndex(CString file);
	int GetStatus(CString &gitdir,CString &path,git_wc_status_kind * status,BOOL IsFull=false, BOOL IsRecursive=false,FIll_STATUS_CALLBACK callback=NULL,void *pData=NULL,CGitHash *pHash=NULL);	
protected:
	int GetFileStatus(CString &gitdir,CString &path, git_wc_status_kind * status,__int64 time,FIll_STATUS_CALLBACK callback=NULL,void *pData=NULL,CGitHash *pHash=NULL);
	int GetDirStatus(CString &gitdir,CString &path, git_wc_status_kind * status,__int64 time,FIll_STATUS_CALLBACK callback=NULL,void *pData=NULL,CGitHash *pHash=NULL);
};

class CGitTreeItem
{
public:
	CString	m_FileName;
	CGitHash	m_Hash;
	int			m_Flags;
};

class CGitHeadFileList:public std::vector<CGitTreeItem>
{
private:
	
	int GetPackRef(CString &gitdir);

public:
	std::map<CString,int> m_Map;
	__time64_t  m_LastModifyTimeHead;
	__time64_t  m_LastModifyTimeRef;
	__time64_t	m_LastModifyTimePackRef;

	CString		m_HeadRefFile;
	CGitHash	m_Head;
	CString		m_HeadFile;
	CString		m_Gitdir;
	CString		m_PackRefFile;

	SharedMutex	m_SharedMutex;

	std::map<CString,CGitHash> m_PackRefMap;

	CGitHash	m_TreeHash; /* buffered tree hash value */

	CGitHeadFileList()
	{
		m_LastModifyTimeHead=0;
		m_LastModifyTimeRef=0;
		m_LastModifyTimePackRef = 0;
	}

	int ReadHeadHash(CString gitdir);
	bool CheckHeadUpdate();

	static int CallBack(const unsigned char *, const char *, int, const char *, unsigned int, int, void *);
	int ReadTree();
};

class CGitHeadFileMap:public std::map<CString,CGitHeadFileList> 
{
public:

	SharedMutex  m_SharedMutex;

	CGitHeadFileMap(){ m_SharedMutex.Init(); }
	~CGitHeadFileMap() { m_SharedMutex.Release(); }

	int GetFileStatus(CString &gitdir,CString &path,git_wc_status_kind * status,BOOL IsFull=false, BOOL IsRecursive=false,FIll_STATUS_CALLBACK callback=NULL,void *pData=NULL);
	int CheckHeadUpdate(CString &gitdir);
	int GetHeadHash(CString &gitdir, CGitHash &hash);

	bool IsHashChanged(CString &gitdir)
	{
		CAutoReadLock lock(&m_SharedMutex);
		if( find(gitdir) == end())
			return false;
		
		CAutoReadLock lock1(&(*this).m_SharedMutex);
		return (*this)[gitdir].m_Head != (*this)[gitdir].m_TreeHash;
	}
};


class CGitIndexFileMap:public std::map<CString,CGitIndexList> 
{
public:
	SharedMutex  m_SharedMutex;

	CGitIndexFileMap(){ m_SharedMutex.Init(); }
	~CGitIndexFileMap() { m_SharedMutex.Release(); }

	int CheckAndUpdateIndex(CString &gitdir,bool *loaded=NULL);

	int GetFileStatus(CString &gitdir,CString &path,git_wc_status_kind * status,BOOL IsFull=false, BOOL IsRecursive=false,FIll_STATUS_CALLBACK callback=NULL,void *pData=NULL,CGitHash *pHash=NULL);
	
	int IsUnderVersionControl(CString &gitdir, CString &path, bool isDir,bool *isVersion);
};

class CGitIgnoreItem
{
public:
	SharedMutex  m_SharedMutex;

	CGitIgnoreItem()
	{
		m_LastModifyTime =0;
		m_pExcludeList =NULL;
	}
	~CGitIgnoreItem()
	{
		if(m_pExcludeList)
			git_free_exclude_list(m_pExcludeList);
		m_pExcludeList=NULL;
	}
	__time64_t  m_LastModifyTime;
	CStringA m_BaseDir;
	EXCLUDE_LIST m_pExcludeList;
	int FetchIgnoreList(CString &projectroot, CString &file);
};

class CGitIgnoreList
{
private:
	bool CheckFileChanged(CString &path);
	int	 FetchIgnoreFile(CString &gitdir, CString &gitignore);
	
	int  CheckIgnore(CString &path,CString &root);

public:
	SharedMutex		m_SharedMutex;
	
	CGitIgnoreList(){ m_SharedMutex.Init(); }
	~CGitIgnoreList() { m_SharedMutex.Release(); }

	std::map<CString, CGitIgnoreItem> m_Map;	

	int	 GetIgnoreFileChangeTimeList(CString &dir, std::vector<__int64> &timelist);
	bool CheckIgnoreChanged(CString &gitdir,CString &path);
	int  LoadAllIgnoreFile(CString &gitdir,CString &path);
	bool IsIgnore(CString &path,CString &root);
};

template<class T>
int GetRangeInSortVector(T &vector,LPTSTR pstr,int len, int *start, int *end, int pos)
{
	if( pos < 0)
	{
		return -1;
	}
	if(start == 0 || end == NULL)
		return -1;

	*start=*end=-1;
	if( _tcsnccmp(vector[pos].m_FileName, pstr,len) != 0)
	{
		for(int i=0;i< vector.size();i++)
		{
			if( _tcsnccmp(vector[i].m_FileName, pstr,len) == 0 )
			{
				if(*start<0)
					*start =i;
				*end =i;
			}
		}
		return -1;
	}else
	{
		*start =0;
		*end = vector.size();

		for(int i=pos;i<vector.size();i++)
		{
			if( _tcsnccmp(vector[i].m_FileName, pstr,len) == 0 )
			{
				*end=i;
			}else
			{
				break;
			}
		}
		for(int i=pos;i>=0;i--)
		{
			if( _tcsnccmp(vector[i].m_FileName, pstr,len) == 0 )
			{
				*start=i;
			}else
			{
				break;
			}
		}
	}
	return 0;
}

template<class T>
int SearchInSortVector(T &vector, LPTSTR pstr, int len)
{
	int end=vector.size()-1;
	int start = 0;
	int mid = (start+end)/2;

	if(vector.size() == 0)
		return -1;

	while(!( start == end && start==mid))
	{
		int cmp;
		cmp = _tcsnccmp( vector[mid].m_FileName,pstr,len );

		if(cmp ==0)
			return mid;

		if(cmp < 0)
		{ 
			start = mid+1;
		}

		if(cmp > 0)
		{
			end=mid;
		}
		mid=(start +end ) /2;

	}

	if(_tcsnccmp( vector[mid].m_FileName,pstr,len ) == 0)
		return mid;

	return -1;
};
#if 0

class CGitStatus
{
protected:
	int GetFileStatus(CString &gitdir,CString &path,git_wc_status_kind * status,BOOL IsFull=false, BOOL IsRecursive=false,FIll_STATUS_CALLBACK callback=NULL,void *pData=NULL);	
public:
	CGitIgnoreList m_IgnoreList;
	CGitHeadFileMap m_HeadFilesMap;
	CGitIndexFileMap m_IndexFilesMap;

	int GetStatus(CString &gitdir,CString &path,git_wc_status_kind * status,BOOL IsFull=false, BOOL IsRecursive=false,FIll_STATUS_CALLBACK callback=NULL,void *pData=NULL);	
};

#endif