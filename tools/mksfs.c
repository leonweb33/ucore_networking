#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <assert.h>


typedef int bool;

#define __error(msg, quit, ...)                                                         \
    do {                                                                                \
        fprintf(stderr, #msg ": function %s - line %d: ", __FUNCTION__, __LINE__);      \
        if (errno != 0) {                                                               \
            fprintf(stderr, "[error] %s: ", strerror(errno));                           \
        }                                                                               \
        fprintf(stderr, "\n\t"), fprintf(stderr, __VA_ARGS__);                          \
        errno = 0;                                                                      \
        if (quit) {                                                                     \
            exit(-1);                                                                   \
        }                                                                               \
    } while (0)

#define warn(...)           __error(warn, 0, __VA_ARGS__)
#define bug(...)            __error(bug, 1, __VA_ARGS__)

#define static_assert(x)                                                                \
    assert(x);

/* 2^31 + 2^29 - 2^25 + 2^22 - 2^19 - 2^16 + 1 */
#define GOLDEN_RATIO_PRIME_32       0x9e370001UL

#define HASH_SHIFT                              10
#define HASH_LIST_SIZE                          (1 << HASH_SHIFT)




#ifdef WIN32
#include <windows.h>
#define S_ISLNK(mode)                   (0)      
#define S_ISSOCK(mode)                  (0)

static ssize_t
pwrite(int d, const void *buf, size_t nbytes, off_t offset)
{
    HANDLE file = (HANDLE)_get_osfhandle(d);
    OVERLAPPED ol;
    memset(&ol, 0, sizeof(ol));
    ol.Offset = offset;
    ol.OffsetHigh = offset >> 32;
    DWORD wrote;
    DWORD status = WriteFile(file, buf, nbytes, &wrote, &ol);
    if (status == 0) {
        //errno = rb_w32_map_errno(GetLastError());
        return -1;
    }

    return wrote;
}

int lstat(const char *path, struct stat *buf) {
    return stat(path, buf);
}


int readlink(const char* path, char* buf, size_t bufsiz) {
    bug("windows have no readlink function\n");
    return -1;
}

#endif // !WIN32





static inline uint32_t
__hash32(uint32_t val, unsigned int bits) {
    uint32_t hash = val * GOLDEN_RATIO_PRIME_32;
    return (hash >> (32 - bits));
}

static uint32_t
hash32(uint32_t val) {
    return __hash32(val, HASH_SHIFT);
}

static uint32_t
hash64(uint64_t val) {
    return __hash32((uint32_t)val, HASH_SHIFT);
}

void *
safe_malloc(size_t size) {
    void *ret;
    if ((ret = malloc(size)) == NULL) {
        bug("malloc %lu bytes failed.\n", (long unsigned)size);
    }
    return ret;
}    

char *
safe_strdup(const char *str) {
    char *ret;
    if ((ret = strdup(str)) == NULL) {
        bug("strdup failed: %s\n", str);
    }
    return ret;
}

struct stat *
safe_stat(const char *filename) {
    static struct stat __stat;
    if (stat(filename, &__stat) != 0) {
        bug("stat %s failed.\n", filename);
    }
    return &__stat;
}

struct stat *
safe_fstat(int fd) {
    static struct stat __stat;
    if (fstat(fd, &__stat) != 0) {
        bug("fstat %d failed.\n", fd);
    }
    return &__stat;
}

struct stat *
safe_lstat(const char *name) {
    static struct stat __stat;
    if (lstat(name, &__stat) != 0) {
        bug("lstat '%s' failed.\n", name);
    }
    return &__stat;
}


void
safe_fchdir(int fd) {
#ifdef WIN32
    bug("windows have no fchdir \n");
#else     
    if (fchdir(fd) != 0) {
        bug("fchdir failed %d.\n", fd);
    }
#endif 
}

void
safe_chdir(const char *filename) {
    if (chdir(filename) != 0) {
        bug("chdir failed : %s.\n", filename);
    }
}

#define SFS_MAGIC                               0x2f8dbe2a
#define SFS_NDIRECT                             12
#define SFS_BLKSIZE                             4096
#define SFS_MAX_NBLKS                           (1024UL * 512)
#define SFS_MAX_INFO_LEN                        31
#define SFS_MAX_FNAME_LEN                       255
#define SFS_MAX_FILE_SIZE                       (1024UL * 1024 * 128)
                                                // 4096 * 8
#define SFS_BLKBITS                             (SFS_BLKSIZE * CHAR_BIT)
#define SFS_TYPE_FILE                           1
#define SFS_TYPE_DIR                            2
#define SFS_TYPE_LINK                           3

#define SFS_BLKN_SUPER                          0
#define SFS_BLKN_ROOT                           1
#define SFS_BLKN_FREEMAP                        2

//目录、文件数据
struct cache_block {
    uint32_t ino; // block number
    struct cache_block *hash_next;
    void *cache; //一个SFS_BLKSIZE的内存空间
};

//目录、文件元数据
struct cache_inode {
    struct inode {
        union {
            struct {
                uint32_t size;
            } fileinfo;
            struct {
                uint32_t slots;
                uint32_t parent;
            } dirinfo;
        } ;
        uint16_t type;
        uint16_t nlinks;
        uint32_t blocks;
        uint32_t direct[SFS_NDIRECT];
        uint32_t indirect;
        //db_indirect 实际上没有用处
        uint32_t db_indirect;
    } inode;
    ino_t real;
    uint32_t ino;
    //本inode中，block的数量
    uint32_t nblks;
    struct cache_block *l1, *l2;
    struct cache_inode *hash_next;
};

struct sfs_fs {
    //写入super_block，第0个block
    struct {
        uint32_t magic;
        uint32_t blocks;
        uint32_t unused_blocks;
        // 文件系统名称 
        char info[SFS_MAX_INFO_LEN + 1];
    } super;
    struct subpath {
        struct subpath *next, *prev;
        char *subname;
    } __sp_nil, *sp_root, *sp_end;
    
    int imgfd;
    // ninos 表示 imgfd 的 block总数
    uint32_t ninos, next_ino;

    struct cache_inode *root;
    // struct cache_inode *inodes[1024];
    struct cache_inode *inodes[HASH_LIST_SIZE];
    // struct cache_block *blocks[1024];
    struct cache_block *blocks[HASH_LIST_SIZE];
};
/*   -----------------                               ________
 *   |  cache_inode* |--> |cache_inode->hash_next|-->|______|
 *   |_______________|                               _______
 *   |_______________|--> |cache_inode->hash_next|-->|______|
 *   |_______________|--> 
 *
 */


//表示文件、目录名称,占一个block
struct sfs_entry {
    uint32_t ino;
    char name[SFS_MAX_FNAME_LEN + 1];
};

static uint32_t
sfs_alloc_ino(struct sfs_fs *sfs) {
    if (sfs->next_ino < sfs->ninos) {
        sfs->super.unused_blocks --;
        return sfs->next_ino ++;
    }
    bug("out of disk space.\n");
}

/*
 * ino 为0 时，重新分配ino
 * 给ino确定一个相应的cache_block 插入到 sfs->blocks中去
 * update_cache
 *   alloc_cache_block
 */
static struct cache_block *
alloc_cache_block(struct sfs_fs *sfs, uint32_t ino) {
    
    struct cache_block *cb = safe_malloc(sizeof(struct cache_block));
    
    cb->ino = (ino != 0) ? ino : sfs_alloc_ino(sfs);
    
    cb->cache = memset(safe_malloc(SFS_BLKSIZE), 0 , SFS_BLKSIZE);
    
    //将cache_block cb插入到sfs->blocks中去
    struct cache_block **head = sfs->blocks + hash32(ino);
    cb->hash_next = *head, *head = cb; // linked
    return cb;
}

/*
 * 在sfs->blocks中查询ino对应的cache_block
 */
struct cache_block *
search_cache_block(struct sfs_fs *sfs, uint32_t ino) {
    //定位到hash表中的一个子链表
    struct cache_block *cb = sfs->blocks[hash32(ino)];

    while(cb != NULL && cb->ino != ino) {
        //下一个
        cb = cb->hash_next;
    }
    return cb;
}

// real用于计算新分配cache_inode在hash表中的位置
static struct cache_inode *
alloc_cache_inode(struct sfs_fs *sfs, ino_t real, uint32_t ino, uint16_t type) {
    
    struct cache_inode *ci = safe_malloc(sizeof(struct cache_inode));
    // 设置inode number
    ci->ino = (ino != 0) ? ino : sfs_alloc_ino(sfs);
    // 设置本inode 的block数量
    ci->real = real, ci->nblks = 0, ci->l1 = ci->l2 = NULL;
    
    struct inode *inode = &(ci->inode);
    memset(inode, 0, sizeof(struct inode));
    // SFS_TYPE_DIR , SFS_TYPE_FILE
    inode->type = type;
    // 将ci放入哈希链表 sfs->inodes中去,放在链表头上
    struct cache_inode **head = sfs->inodes + hash64(real);
    ci->hash_next = *head, *head = ci;
    return ci;
}

/*
 * 在sfs->inodes中查找 real对应的inode
 */
struct cache_inode *
search_cache_inode(struct sfs_fs *sfs, ino_t real) {
    struct cache_inode *ci = sfs->inodes[hash64(real)];
    while (ci != NULL && ci->real != real) {
        // 下一个
        ci = ci->hash_next;
    }
    return ci;
}

static void
init_dir_cache_inode(struct cache_inode *current, struct cache_inode *parent) {
    struct inode *inode = &(current->inode);
    assert(inode->type == SFS_TYPE_DIR && parent->inode.type == SFS_TYPE_DIR);
    assert(inode->nlinks == 0 && inode->dirinfo.slots == 0 && inode->dirinfo.parent == 0);
    inode->nlinks ++ , parent->inode.nlinks ++, inode->dirinfo.parent = parent->ino; 
}



struct sfs_fs *
create_sfs(int imgfd) {
    uint32_t ninos, next_ino;// ninos表示总共的block数量，next_ino表示下一个可用的inode编号
    struct stat *stat =safe_fstat(imgfd);
                                 //4096
    if ((ninos = stat->st_size / SFS_BLKSIZE) > SFS_MAX_NBLKS) {
        ninos = SFS_MAX_NBLKS;
        warn("img file is too big (%llu bytes, only use %u blocks).\n",
                (unsigned long long)stat->st_size, ninos);
    }

    //freemap中一个bit表示一个block的状态,一个需要ninos个bit来表示disk0的所有block状态
    // SFS_BLKN_FREEMAP == 2
    // SFS_BLKBITS = 8 *4096 = 32768
    // 2 + ( 32768 + 8 * 4096 -1 ) / 8 * 4096 = 3
    if ((next_ino = SFS_BLKN_FREEMAP + (ninos + SFS_BLKBITS - 1) / SFS_BLKBITS) >= ninos) {
        bug("img file is too small (%llu bytes, %u blocks, bitmap use at least %u blocks).\n",
                (unsigned long long)stat->st_size, ninos, next_ino - 2);
    }
    
    /*
    printf("st_size:%d\n", stat->st_size);
    printf("ninos:%d\n",ninos);
    printf("next_ino:%d\n",next_ino );
    printf("(ninos + SFS_BLKBITS - 1) / SFS_BLKBITS) = %d\n",(ninos + SFS_BLKBITS - 1) / SFS_BLKBITS );
    */
    
    struct sfs_fs *sfs = safe_malloc(sizeof(struct sfs_fs));
    //设置 super block
    sfs->super.magic = SFS_MAGIC;
    sfs->super.blocks = ninos, sfs->super.unused_blocks = ninos - next_ino;
    snprintf(sfs->super.info, SFS_MAX_INFO_LEN, "simple file system");
    
    sfs->ninos = ninos, sfs->next_ino = next_ino, sfs->imgfd = imgfd;
    
    sfs->sp_root = sfs->sp_end = &(sfs->__sp_nil);//
    sfs->sp_end->prev = sfs->sp_end->next = NULL;//

    int i;
    for (i = 0; i < HASH_LIST_SIZE; i ++) {
        sfs->inodes[i] = NULL;
        sfs->blocks[i] = NULL;
    }
    
    //分配root 目录分配cache_inode      0,      1
    sfs->root = alloc_cache_inode(sfs, 0, SFS_BLKN_ROOT, SFS_TYPE_DIR);
    init_dir_cache_inode(sfs->root, sfs->root);
    return sfs;
}

/*
 *                            <- old sfs->sp_next
 * sfs->sp_end=subpath [prev] -> old [sfs->sp_end] 
 *
 */

static void
subpath_push(struct sfs_fs *sfs, const char *subname) {
    struct subpath *subpath = safe_malloc(sizeof(struct subpath));
    subpath->subname = safe_strdup(subname);

    sfs->sp_end->next = subpath;
    subpath->prev = sfs->sp_end;
    subpath->next = NULL;
    sfs->sp_end = subpath;
}

static void
subpath_pop(struct sfs_fs *sfs) {
    assert(sfs->sp_root != sfs->sp_end);//sfs->sp_end不为nil
    struct subpath *subpath = sfs->sp_end;
    sfs->sp_end = sfs->sp_end->prev, sfs->sp_end->next = NULL;
    free(subpath->subname), free(subpath);
}

static void
subpath_show(FILE *fout, struct sfs_fs *sfs, const char *name) {
    struct subpath *subpath = sfs->sp_root;
    fprintf(fout, "current is: /");
    while((subpath = subpath->next) != NULL) {
        fprintf(fout, "%s/", subpath->subname);
    }
    if (name != NULL) {
        fprintf(fout, "%s", name);
    }
    fprintf(fout, "\n");
}

//将data写入第ino个block
static void
write_block(struct sfs_fs *sfs, void *data, size_t len, uint32_t ino) {
    
    assert(len <= SFS_BLKSIZE && ino < sfs->ninos);
    static char buffer[SFS_BLKSIZE];
    
    if (len != SFS_BLKSIZE) {
        //设置要写到磁盘上去的数据
        memset(buffer, 0, sizeof(buffer));
        data = memcpy(buffer, data, len);
    }
    //写入磁盘的偏移位置  ino * 512
    off_t offset = (off_t)ino * SFS_BLKSIZE;
    ssize_t ret;
    if ((ret = pwrite(sfs->imgfd, data, SFS_BLKSIZE, offset)) != SFS_BLKSIZE) {
        bug("write %u block failed: (%d/%d).\n", ino, (int)ret, SFS_BLKSIZE);
    }
}

// write cache_block to disk
static void
flush_cache_block(struct sfs_fs *sfs, struct cache_block *cb) {
    write_block(sfs, cb->cache, SFS_BLKSIZE, cb->ino);
}

// write cache_inode to disk
static void
flush_cache_inode(struct sfs_fs *sfs, struct cache_inode *ci) {
    write_block(sfs, &(ci->inode), sizeof(ci->inode), ci->ino);
}

// 
void
close_sfs(struct sfs_fs *sfs) {
    static char buffer[SFS_BLKSIZE];

    uint32_t i, j , ino =SFS_BLKN_FREEMAP;
    uint32_t ninos = sfs->ninos, next_ino = sfs->next_ino;
    
    // 这一段好奇怪
    for (i = 0; i < ninos; ino ++, i += SFS_BLKBITS) {
        memset(buffer, 0, sizeof(buffer));
        if (i + SFS_BLKBITS > next_ino) {
            uint32_t start = 0, end = SFS_BLKBITS;
            if (i < next_ino) {
                start = next_ino - i;
            }
            if (i + SFS_BLKBITS > ninos) {
                end = ninos - i;
            }
            uint32_t *data = (uint32_t *)buffer;
            const uint32_t bits = sizeof(bits) * CHAR_BIT;
            for (j = start; j < end; j ++) {
                data[j / bits] |= (1 << (j % bits));
            }
        }
        write_block(sfs, buffer, sizeof(buffer), ino);
    }
    //super block 写到disk
    write_block(sfs, &(sfs->super), sizeof(sfs->super), SFS_BLKN_SUPER);
    

    for (i = 0; i < HASH_LIST_SIZE; i ++) {
        struct cache_block *cb = sfs->blocks[i];
        while(cb != NULL) {
            flush_cache_block(sfs, cb);
            cb = cb->hash_next;
        }
        struct cache_inode *ci = sfs->inodes[i];
        while(ci != NULL) {
            flush_cache_inode(sfs, ci);
            ci = ci->hash_next;
        }
    }
}

struct sfs_fs *
open_img(const char *imgname) {
    // imgname = bin/sfs.img
    const char *expect = ".img", *ext = imgname + strlen(imgname) - strlen(expect);
    if (ext <= imgname || strcmp(ext, expect) != 0) {
        bug("invalid .img file name '%s'.\n", imgname);
    }
    int imgfd;
    if ((imgfd = open(imgname, O_WRONLY)) < 0) {
        bug("open '%s' failed.\n", imgname);
    }
    return create_sfs(imgfd);
}

#define open_bug(sfs, name, ...)                                                        \
    do {                                                                                \
        subpath_show(stderr, sfs, name);                                                \
        bug(__VA_ARGS__);                                                               \
    } while (0)

#define show_fullpath(sfs, name) subpath_show(stderr, sfs, name)

void open_dir(struct sfs_fs *sfs, struct cache_inode *current, struct cache_inode *parent);
void open_file(struct sfs_fs *sfs, struct cache_inode *file, const char *filename, int fd);
void open_link(struct sfs_fs *sfs, struct cache_inode *file, const char *filename);

                                                // 4096 / 4 = 1024
#define SFS_BLK_NENTRY                          (SFS_BLKSIZE / sizeof(uint32_t))
                                                // 12
#define SFS_L0_NBLKS                            SFS_NDIRECT
                                                // 1024  + 12
#define SFS_L1_NBLKS                            (SFS_BLK_NENTRY + SFS_L0_NBLKS)
                                                // 1024 * 1024 + (1024 + 12)
#define SFS_L2_NBLKS                            (SFS_BLK_NENTRY * SFS_BLK_NENTRY + SFS_L1_NBLKS)
                                                // (1024UL * 1024 * 128) /  4096,cache_inode->blocks最大不能超过这个数量
#define SFS_LN_NBLKS                            (SFS_MAX_FILE_SIZE / SFS_BLKSIZE)


/*
 * 根据inop 分配或者查找到一个cache_blcok
 * (*inop == 0)是，新分配cache_block和 ino
 */
static void
update_cache(struct sfs_fs *sfs, struct cache_block **cbp, uint32_t *inop) {
    uint32_t ino = *inop;
    struct cache_block *cb = *cbp;
    if (ino == 0) {
        //alloc_cache_block 重新确定 ino的
        cb = alloc_cache_block(sfs, 0);
        ino = cb->ino;
    }
    else if (cb == NULL || cb->ino != ino) {
        cb = search_cache_block(sfs, ino);
        assert(cb != NULL && cb->ino == ino);
    }
    //分配的信息，带出去
    *cbp = cb , *inop = ino;
}

/*
 *
 * 给 cache_inode file 添加一个cache_block
 * 将 block id = ino的block标记到 file->inode->direct 或者 file->inode->indirect 或者 file->inode->db_indirect中去
 */
static void
__append_block(struct sfs_fs *sfs, struct cache_inode *file, uint32_t ino, const char *filename) {
    static_assert(SFS_LN_NBLKS <= SFS_L2_NBLKS);
    
    uint32_t nblks = file->nblks;
    
    struct inode *inode = &(file->inode);
    
    if (nblks >= SFS_LN_NBLKS) {
        open_bug(sfs, filename, "file is too big.\n");
    }
    
    if (nblks < SFS_L0_NBLKS) {
        // 第一级
        inode->direct[nblks] = ino;
    }
    else if (nblks < SFS_L1_NBLKS) {
        // 第二级
        nblks -= SFS_L0_NBLKS;
        
        // 给inode->indirect分配一个 block,用于存放 扩张的 block id
        // inode->indirect其实就是file->l1 这个block的id
        update_cache(sfs, &(file->l1), &(inode->indirect));
        
        uint32_t *data = file->l1->cache;
        data[nblks] = ino;
    }
    else if (nblks < SFS_L2_NBLKS) {
        // 第三级
        nblks -= SFS_L1_NBLKS;
        update_cache(sfs, &(file->l2), &(inode->db_indirect));
        
        uint32_t *data2 = file->l2->cache;
        update_cache(sfs, &(file->l1), &data2[nblks / SFS_BLK_NENTRY]);
        
        uint32_t *data1 = file->l1->cache;
        data1[nblks % SFS_BLK_NENTRY] = ino;
    }
    
    file->nblks ++;
    inode->blocks ++;
}

static void
append_block_size(struct sfs_fs *sfs, struct cache_inode *file, size_t size, uint32_t ino, const char *filename) {
    struct inode *inode = &(file->inode);
    assert(inode->type != SFS_TYPE_DIR && size != 0 && size <= SFS_BLKSIZE);
    assert(inode->fileinfo.size % SFS_BLKSIZE == 0);
    inode->fileinfo.size += size;
    __append_block(sfs, file, ino, filename);
}


static void
append_block_slot(struct sfs_fs *sfs, struct cache_inode *file, uint32_t ino, const char *filename) {
    struct inode *inode = &(file->inode);
    assert(inode->type == SFS_TYPE_DIR);
    inode->dirinfo.slots ++;
    __append_block(sfs, file, ino, filename);
}

/*
 * 给目录 current添加一个 sfs_entry, sfs_entry->ino 指向 file
 */
static void
add_entry(struct sfs_fs *sfs, struct cache_inode *current, struct cache_inode *file, const char *name) {
    //sfs_disk_entry
    static struct sfs_entry __entry, *entry = &__entry;
    //current必须是目录类型的inode
    assert(current->inode.type == SFS_TYPE_DIR && strlen(name) <= SFS_MAX_FNAME_LEN);
    //设置block number id和文件名称
    entry->ino = file->ino, strcpy(entry->name, name);
    // 下一个 block ino
    uint32_t entry_ino = sfs_alloc_ino(sfs);
    // 都是从 entry_no * blocksize开始写，这个意思是 一个entry占有一个disk block 了
    //               data ,  len=256          , block number id 
    write_block(sfs, entry, sizeof(entry->name), entry_ino);
    
    // printf("entry->name: %s entry_ino: %d  len:%d \n", entry->name, entry_ino ,sizeof(entry->name));
    
    // 在sfs->blocks中添加一个cache_block和相应的ino
    // 就是将entry_ino 记录到current->inode->direct 或则 current->inode->indirect 或者 current->inode->db_indirect
    //current->inode->direct[current->nblks] = entry_ino
    // 给目录 current 添加一个表示 文件或者目录的sfs_entry数据
    append_block_slot(sfs, current, entry_ino, name);
    //append_block(sfs, current, sizeof(entry->name), entry_ino, name);
    file->inode.nlinks ++;
}

/*
static void
add_dir(struct sfs_fs *sfs, struct cache_inode *parent, const char *dirname, int curfd, int fd, ino_t real) {
    assert(search_cache_inode(sfs, real) == NULL);
    
    //给新的目录分配一个cache_inode
    struct cache_inode *current = alloc_cache_inode(sfs, real, 0, SFS_TYPE_DIR);
    init_dir_cache_inode(current, parent);
    //open_dir 递归下去
    safe_fchdir(fd), subpath_push(sfs, dirname);
    open_dir(sfs, current, parent);
    safe_fchdir(curfd), subpath_pop(sfs);
    //当前目录信息的inode需要一个 block
    add_entry(sfs, parent, current, dirname);
}
*/

/*
 * 
 *
 *  name = direntp->d_name
 *  real = stat.st_ino
 */
static void
add_file(struct sfs_fs *sfs, struct cache_inode *current, const char *filename, int fd, ino_t real) {
    struct cache_inode *file;
    if ((file = search_cache_inode(sfs, real)) == NULL) {
        //没有添加过，开始添加
        //分配一个 cache_inode,
        file = alloc_cache_inode(sfs, real, 0, SFS_TYPE_FILE);
        //将文件filename的信息写入block，并且将block id 填入 file->inode->direct中
        open_file(sfs, file, filename, fd);
    }
    //当前文件信息本身的inode需要一个block
    add_entry(sfs, current, file, filename);

    /*
    printf("file->ino: %d  direct : ",file->ino);
    int i =0;
    for(i=0; i < file->inode.blocks; i++ ) {
        printf("%d ",file->inode.direct[i]);
    }
    printf("\n");
    */
}

/*
 * 
 *
 *  name = direntp->d_name
 *  real = stat.st_ino
 */
static void
add_link(struct sfs_fs *sfs, struct cache_inode *current, const char *filename, ino_t real) {
    struct cache_inode *file = alloc_cache_inode(sfs, real, 0, SFS_TYPE_LINK);
    open_link(sfs, file, filename);
    add_entry(sfs, current, file, filename);
}

/*
 *
 * open_dir(sfs, sfs->root, sfs->root)
 *
 */
void
open_dir(struct sfs_fs *sfs, struct cache_inode *current, struct cache_inode *parent) {
    DIR *dir;
    if ((dir = opendir(".")) == NULL) {
        open_bug(sfs, NULL, "opendir failed.\n");
    }
    add_entry(sfs, current, current, ".");
    add_entry(sfs, current, parent, "..");
    
    struct dirent *direntp;
    static int space = 0;
    while((direntp = readdir(dir)) != NULL) {
        const char *name =direntp->d_name;
        
        if (strcmp(name, ".") == 0 || strcmp(name , "..") == 0) {
            //跳过 "."和".."
            continue;
        }
        if (name[0] == '.') {
            continue;
        }
        if (strlen(name) > SFS_MAX_FNAME_LEN) {
            open_bug(sfs, NULL, "file name is too long: %s\n", name);
        }
        struct stat *stat = safe_lstat(name);
        
        char *spacestr = safe_malloc(SFS_BLKSIZE);
        memset(spacestr,0,SFS_BLKSIZE);
        memset(spacestr,' ',space);
        // printf("%sname: %s    st_ino:%d  st_size:%x  blocks:%d\n",spacestr,name,stat->st_ino, stat->st_size, (stat->st_size+4096) / 4096);
        free(spacestr);
        
        
        if (S_ISLNK(stat->st_mode)) {
            add_link(sfs, current, name, stat->st_ino);
        }
        else {
            int fd;
            if ((fd = open(name, O_RDONLY)) < 0) {
                open_bug(sfs, NULL, "open failed: %s\n", name);
            }
            if (S_ISDIR(stat->st_mode)) {
                space += 4;
                bug("shouldn't have dir on disk0\n");
                //add_dir(sfs, current, name, dirfd(dir), fd, stat->st_ino);
                space -=4;
            }
            else if (S_ISREG(stat->st_mode)) {
                add_file(sfs, current, name, fd, stat->st_ino);
            }
            else {
                char mode = '?';
                if (S_ISFIFO(stat->st_mode)) mode = 'f';
                if (S_ISSOCK(stat->st_mode)) mode = 's';
                if (S_ISCHR(stat->st_mode)) mode = 'c';
                if (S_ISBLK(stat->st_mode)) mode = 'b';
                show_fullpath(sfs, NULL);
                warn("unsupported mode %07x (%c): file %s\n", stat->st_mode, mode, name);
            }
            close(fd);
        }
    }
    closedir(dir);
}

void
open_file(struct sfs_fs *sfs, struct cache_inode *file, const char *filename, int fd) {
    static char buffer[SFS_BLKSIZE];
    ssize_t ret, last = SFS_BLKSIZE;
    //每次处理一个block的量
    while((ret = read(fd, buffer, sizeof(buffer))) != 0) {
        assert(last == SFS_BLKSIZE);
        uint32_t ino = sfs_alloc_ino(sfs); //数据block的 block id 
        //将数据buffer写到第ino个block上去
        write_block(sfs, buffer, ret, ino); 
        //将第ino个block的id(ino)添加到file->inode->direct 或者 file->inode->in_direct中去
        append_block_size(sfs, file, ret, ino, filename);
        last = ret;
    }
    if (ret < 0) {
        open_bug(sfs, filename, "read file failed.\n");
    }
}

void
open_link(struct sfs_fs *sfs, struct cache_inode *file, const char *filename) {
    static char buffer[SFS_BLKSIZE];
    uint32_t ino = sfs_alloc_ino(sfs);
    ssize_t ret = readlink(filename, buffer, sizeof(buffer));
    if (ret < 0 || ret == SFS_BLKSIZE) {
        open_bug(sfs, filename, "read link failed, %d", (int)ret);
    }
    write_block(sfs, buffer, ret, ino);
    append_block_size(sfs, file, ret, ino, filename);
}

// home= disk0
int
create_img(struct sfs_fs *sfs, const char *home) {
    int curfd, homefd;
    if ((curfd = open(".", O_RDONLY)) < 0) {
        bug("get current fd failed.\n");
    }
    if ((homefd = open(home, O_RDONLY /*|O_NOFOLLOW 如果参数pathname所指的文件为一符号连接，则会令打开文件失败*/ )) < 0) {
        bug("open home directory '%s' failed.\n", home);
    }
    //修改当前目录到 disk0去
    //safe_fchdir(homefd);
    safe_chdir(home);

    open_dir(sfs, sfs->root, sfs->root);
    //返回到当前目录 .
    //safe_fchdir(curfd);
    safe_chdir(".");
    close(curfd), close(homefd);
    close_sfs(sfs);
    return 0;
}

static void
static_check(void) {
    //static_assert(sizeof(off_t) == 8);
    //static_assert(sizeof(ino_t) == 8);
    static_assert(SFS_MAX_NBLKS <= 0x80000000UL);
    static_assert(SFS_MAX_FILE_SIZE <= 0x80000000UL);
}

int
main(int argc, char **argv) {
    static_check();
    if (argc != 3) {
        bug("usage: <input *.img> <input dirname>\n");
    }
    const char *imgname = argv[1], *home = argv[2];
    // imgname= bin/sfs.img
    // home = disk0
    if (create_img(open_img(imgname), home) != 0) {
        bug("create img failed.\n");
    }
    printf("create %s (%s) successfully.\n", imgname, home);
    return 0;
}
