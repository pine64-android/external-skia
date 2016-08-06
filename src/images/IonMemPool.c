#include "IonMemPool.h"
#include "memoryAdapter.h"
#include "log.h"

#define USE_POOL (1)

typedef struct tagIS
{
	struct tagIS 	*next;
	void* 			data;
}ItemSlot;

struct _tag_array
{
	struct tagIS 	*head;
	struct tagIS 	*tail;
	unsigned int 	entryCount;       // number of ItemSlot in the list
	int 			foundEntryNumber;
	struct tagIS 	*foundEntry;
};

typedef struct _tag_array IMPoolList;


struct NodeItem
{
	int size;
	int usedFlag;
	int type; // outputIndex,  input_index, fbm
	unsigned char* buf; // ion alloc buffer addr
};

struct IonMemPoolInstance 
{
	int             totalSize;
	int             NodeNum;
	IMPoolList*     MemList;
	pthread_mutex_t listMutex;
	
};

/*
extern "C"
{
extern void MemAdapterFlushCache(void * ptr,int size);
extern void MemAdapterPfree(void* pMem);
extern void* MemAdapterPalloc(int nSize);
extern void* AdapterMemGetPhysicAddress(void* pVirtualAddress);
}*/


void* IMPoolListLast(IMPoolList* ptr)
{
	ItemSlot* entry;

	if(!ptr || !ptr->entryCount) return NULL;
	entry = ptr->head;
	
	while(entry->next)
	{
		entry = entry->next;
	}

	return entry->data;
}

unsigned int IMPoolListCount(IMPoolList* ptr)
{
	if(!ptr) return -1;
	return ptr->entryCount;
}

int IMPoolListRem(IMPoolList* ptr, unsigned int itemNumber)
{
	ItemSlot *tmp, *tmp2;
	if(!ptr || (!ptr->entryCount) || (!ptr->head) || (itemNumber >= ptr->entryCount)) 
		return -1;

	//delete head
	if(!itemNumber)
	{
		tmp = ptr->head;
		ptr->head = ptr->head->next;
		ptr->entryCount--;
		ptr->foundEntry = ptr->head;
		ptr->foundEntryNumber = 0;
		free(tmp);

		// that was the last entry, reset the tail
		if(!ptr->entryCount)
		{
			ptr->tail = ptr->head = ptr->foundEntry = NULL;
			ptr->foundEntryNumber = -1;
		}
		return 0;
	}

	tmp = ptr->head;
	unsigned int i;
	for(i = 0; i < itemNumber-1; i++)
	{
		tmp = tmp->next;
	}
	tmp2 = tmp->next;
	tmp->next = tmp2->next;
	/*if we deleted the last entry, update the tail !!!*/
	if (! tmp->next || (ptr->tail == tmp2) ) {
		ptr->tail = tmp;
		tmp->next = NULL;
	}
	
	free(tmp2);
	ptr->entryCount -- ;
	ptr->foundEntry = ptr->head;
	ptr->foundEntryNumber = 0;

	return 0;
}


void IMPoolListDel(IMPoolList *ptr)
{
	if (!ptr) return;
	while (ptr->entryCount) IMPoolListRem(ptr, 0);
	free(ptr);
}


int IMPoolListRemLast(IMPoolList *ptr)
{
	return IMPoolListRem(ptr, ptr->entryCount-1);
}

void IMPoolListFree(IMPoolList* list, void(*_free)(void*))
{
	if(!list)	return;

	while(IMPoolListCount(list))
	{
		void* item = IMPoolListLast(list);
		IMPoolListRemLast(list);
		if(item && _free) _free(item);
	}

	IMPoolListDel(list);
}




IMPoolList* IMPoolListNew()
{
	IMPoolList* mlist = (IMPoolList*)malloc(sizeof(IMPoolList));
	if(!mlist) return NULL;
	
	mlist->head = mlist->foundEntry = NULL;
	mlist->tail = NULL;
	mlist->foundEntryNumber = -1;
	mlist->entryCount = 0;
	return mlist;
}





void IMPoolListReset(IMPoolList *ptr)
{
	while (ptr && ptr->entryCount) IMPoolListRem(ptr, 0);
}

int IMPoolListAdd(IMPoolList* ptr, void* item)
{
	ItemSlot *entry;
	if(!ptr) 
	{
		printf(" parameter error. \n");
		return -1;
	}
	entry = (ItemSlot*)malloc(sizeof(ItemSlot));
	if(!entry) return -1;

	entry->data = item;
	entry->next = NULL;

	if(!ptr->head)
	{
		ptr->head = entry;
		ptr->entryCount = 1;
	}
	else
	{
		ptr->entryCount += 1;
		ptr->tail->next = entry;
	}
	ptr->tail = entry;
	ptr->foundEntryNumber = ptr->entryCount - 1;
	ptr->foundEntry = entry;
	return 0;
}



void* IMPoolListGet(IMPoolList *ptr, unsigned int itemNumber)
{
	ItemSlot* entry;
	unsigned int i;
	if(!ptr || (itemNumber >= ptr->entryCount)) return NULL;
	
	//if it is the first time to get item, get the first item
	if(!ptr->foundEntry || (itemNumber<(unsigned int)ptr->foundEntryNumber))
	{
		ptr->foundEntryNumber = 0;
		ptr->foundEntry = ptr->head;
	}
	entry = ptr->foundEntry;
	for(i = ptr->foundEntryNumber; i< itemNumber; ++i)
	{
		entry = entry->next;
	}
	ptr->foundEntryNumber = itemNumber;
	ptr->foundEntry = entry;

	return (void*) entry->data; 
	
}

int IMPoolListInsert(IMPoolList *ptr, void* item, unsigned int position)
{
	if(!ptr || !item) return -1;

	//add the item to the end of list
	if(position >= ptr->entryCount) 
		return IMPoolListAdd(ptr, item);

	ItemSlot *entry;
	entry = (ItemSlot*)malloc(sizeof(ItemSlot));
	entry->data = item;
	entry->next = NULL;

	// insert in head of list
	if (position == 0)
	{
		entry->next = ptr->head;
		ptr->head = entry;
		ptr->entryCount++;
		ptr->foundEntry = entry;
		ptr->foundEntryNumber = 0;
		return 0;
	}
	
	unsigned int i;
	ItemSlot* tmp = ptr->head;
	for(i=0; i<position-1; i++)
	{
		tmp = tmp->next;
	}
	entry->next = tmp->next;
	tmp->next = entry;
	ptr->entryCount++;
	ptr->foundEntry = entry;
	ptr->foundEntryNumber = i;
	return 0;
}

//********************** list end ******************************

static pthread_mutex_t gIMPoolMutex = PTHREAD_MUTEX_INITIALIZER;

static struct IonMemPoolInstance *IMPCreateInstance()
{
	struct IonMemPoolInstance *instance;
	instance =(struct IonMemPoolInstance *)malloc(sizeof(struct IonMemPoolInstance));
	
	instance->totalSize = 0;
	instance->NodeNum = 0;
	instance->MemList = IMPoolListNew();
	pthread_mutex_init(&instance->listMutex, NULL);

	if(MemAdapterOpen() < 0)
	{
		loge("memadapterOpen failed");
		return NULL;
	}
	
	return instance;
}

static struct IonMemPoolInstance* IMPGetInstatnce()
{
	
	static struct IonMemPoolInstance* singletonInstance = NULL;

	if(singletonInstance == NULL)
	{
		pthread_mutex_lock(&gIMPoolMutex);
		if(singletonInstance == NULL)
		{
			singletonInstance = IMPCreateInstance();
		}
		pthread_mutex_unlock(&gIMPoolMutex);
	}
	
	return singletonInstance;
}

#if USE_POOL
void* IMPoolPalloc(int nSize)
{
	struct IonMemPoolInstance* instance = NULL;
	struct NodeItem* nodeItem = NULL;
	int nodeNum;
	int i;

	if(nSize <= 0)
	{
		return NULL;
	}

	instance = IMPGetInstatnce();

	pthread_mutex_lock(&instance->listMutex);
	nodeNum = IMPoolListCount(instance->MemList);
	if(instance->totalSize > 0)
	{
		for(i=0; i<nodeNum; i++)
		{
			nodeItem = (struct NodeItem*)IMPoolListGet(instance->MemList, i);

			// find the buffer which is not using now
			if(nodeItem->usedFlag == 0 && nodeItem->size >= nSize)
			{
				nodeItem->usedFlag = 1;
				//logd("+++++++++ IMPoolPalloc get buffer in list , size: %d, buf: 0x%p, phy_addr: 0x%p, instance: %p", nSize, nodeItem->buf, AdapterMemGetPhysicAddress(nodeItem->buf), instance);
				pthread_mutex_unlock(&instance->listMutex);
				return nodeItem->buf;
			}
		}
	}

	// we should ionAlloc a new buf if we cannot find apprite buf in list
	nodeItem = (struct NodeItem*)malloc(sizeof(struct NodeItem));
	if(!nodeItem)
	{
		loge("malloc failed");
		pthread_mutex_unlock(&instance->listMutex);
		return NULL;
	}
	memset(nodeItem, 0x00, sizeof(struct NodeItem));

	nodeItem->buf = (unsigned char*)MemAdapterPalloc(nSize);
	if(!nodeItem->buf)
	{
		loge("palloc failed");
		free(nodeItem);
		pthread_mutex_unlock(&instance->listMutex);
		return NULL;
	}
	nodeItem->size = nSize;
	nodeItem->usedFlag = 1;

	if(IMPoolListAdd(instance->MemList, nodeItem) < 0)
	{
		pthread_mutex_unlock(&instance->listMutex);
		return NULL;
	}

	instance->totalSize += nSize;
	
	pthread_mutex_unlock(&instance->listMutex);

	logd("+++++++++ IMPoolPalloc, size: %d, buf: 0x%p, phy_addr: 0x%p", nSize, nodeItem->buf, MemAdapterGetPhysicAddress(nodeItem->buf));
	return nodeItem->buf;
}

void IMPoolPfree(void* pMem)
{
	struct IonMemPoolInstance* instance = NULL;
	struct NodeItem* nodeItem = NULL;
	int nodeNum;
	int i;

	if(!pMem)
	{
		return;
	}

	instance = IMPGetInstatnce();

	pthread_mutex_lock(&instance->listMutex);

	//* 1, set the node of pMem usedFlag to 0
	nodeNum = IMPoolListCount(instance->MemList);
	for(i=0; i<nodeNum; i++)
	{
		nodeItem = (struct NodeItem*)IMPoolListGet(instance->MemList, i);
		if(nodeItem->buf == pMem)
		{
			nodeItem->usedFlag = 0;
		}
	}

	//* 2, check if the pool size is large than 20M, free a unused node of the list
	if(instance->totalSize > 20*1024*1024)
	{
		for(i=0; i<nodeNum; i++)
		{
			nodeItem = (struct NodeItem*)IMPoolListGet(instance->MemList, i);
			if(nodeItem->usedFlag == 0)
			{
				logd("+++++++++++ free buf: 0x%p", nodeItem->buf);
				MemAdapterPfree(nodeItem->buf);
				instance->totalSize -= nodeItem->size;

				free(nodeItem);
				IMPoolListRem(instance->MemList, i);
				break;
			}
		}
	}
	

	pthread_mutex_unlock(&instance->listMutex);
}
#else

void* IMPoolPalloc(int nSize)
{
	return MemAdapterPalloc(nSize);
}

void IMPoolPfree(void* pMem)
{
	MemAdapterPfree(pMem);
}

#endif

void IMPoolFlushCache(void* pMem, int nSize)
{
	MemAdapterFlushCache(pMem, nSize);
}

