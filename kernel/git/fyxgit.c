/* FYXOS — FYXGit v1：小型、原子持久化的原生版本控制
 *
 * 这是一个明确的 FYXGit 格式，不冒充 Git 的 SHA-1/index/pack 文件格式。
 * 一个 .fyxgit 文件同时保存暂存区、提交父链和内容池；每次修改由
 * vfs_write_file() 一次性发布，工作区文件则保持普通 FYXFS 文件。
 */
#include <fyxos/fyxgit.h>
#include <fyxos/fyxfs.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>
#include <fyxos/vfs.h>

#define FG_VERSION       1u
#define FG_MAX_FILES     8u
#define FG_MAX_COMMITS   8u
#define FG_PATH_CAP      80u
#define FG_ROOT_CAP      96u
#define FG_MESSAGE_CAP   48u
#define FG_BLOB_CAP      4096u
#define FG_POOL_CAP      65536u
#define FG_DEFAULT_ROOT  "/home/user"
#define FG_REPO_NAME     ".fyxgit"

struct fg_blob {
    char path[FG_PATH_CAP];
    u32 offset;
    u32 size;
    u32 crc;
    u32 flags;
};

struct fg_commit {
    u32 id;
    u32 parent;
    u32 count;
    char message[FG_MESSAGE_CAP];
    struct fg_blob files[FG_MAX_FILES];
};

struct fg_repo {
    char magic[8];
    u32 version;
    u32 head;
    u32 commit_count;
    u32 index_count;
    u32 pool_used;
    char root[FG_ROOT_CAP];
    struct fg_blob index[FG_MAX_FILES];
    struct fg_commit commits[FG_MAX_COMMITS];
    u8 pool[FG_POOL_CAP];
    u32 checksum;
};

struct fg_line {
    const u8 *data;
    size_t length;
};

static int fg_normalize_root(const char *input, char *out)
{
    if (!input || !*input)
        input = FG_DEFAULT_ROOT;
    size_t length = strlen(input);
    if (length == 0 || length >= FG_ROOT_CAP || input[0] != '/')
        return -FYX_EINVAL;
    memcpy(out, input, length + 1);
    while (length > 1 && out[length - 1] == '/')
        out[--length] = '\0';
    /* 仓库根不接受路径穿越；斜杠重复可以安全地交给 VFS 处理。 */
    size_t start = 1;
    for (size_t i = 1;; i++) {
        if (out[i] == '/' || out[i] == '\0') {
            size_t n = i - start;
            if ((n == 1 && out[start] == '.') ||
                (n == 2 && out[start] == '.' && out[start + 1] == '.'))
                return -FYX_EINVAL;
            if (out[i] == '\0')
                break;
            start = i + 1;
        }
    }
    return FYX_OK;
}

static int fg_repo_path(const char *root, char *out)
{
    int n;
    if (strcmp(root, "/") == 0)
        n = snprintf(out, VFS_PATH_MAX, "/%s", FG_REPO_NAME);
    else
        n = snprintf(out, VFS_PATH_MAX, "%s/%s", root, FG_REPO_NAME);
    return n < 0 || n >= (int)VFS_PATH_MAX ? -FYX_ERANGE : FYX_OK;
}

static int fg_join(const char *root, const char *relative, char *out)
{
    int n;
    if (strcmp(root, "/") == 0)
        n = snprintf(out, VFS_PATH_MAX, "/%s", relative);
    else
        n = snprintf(out, VFS_PATH_MAX, "%s/%s", root, relative);
    return n < 0 || n >= (int)VFS_PATH_MAX ? -FYX_ERANGE : FYX_OK;
}

static bool fg_valid_relative(const char *path)
{
    if (!path || !*path || path[0] == '/' || strlen(path) >= FG_PATH_CAP)
        return false;
    size_t start = 0;
    for (size_t i = 0;; i++) {
        if (path[i] == '/' || path[i] == '\0') {
            size_t n = i - start;
            if (n == 0 || (n == 1 && path[start] == '.') ||
                (n == 2 && path[start] == '.' && path[start + 1] == '.') ||
                (n == strlen(FG_REPO_NAME) &&
                 memcmp(path + start, FG_REPO_NAME, n) == 0))
                return false;
            if (path[i] == '\0')
                break;
            start = i + 1;
        }
    }
    return true;
}

static int fg_relative(const char *root, const char *argument,
                       char *relative, char *full)
{
    if (!argument || !*argument)
        return -FYX_EINVAL;
    const char *rel = argument;
    if (argument[0] == '/') {
        size_t n = strlen(root);
        if (strcmp(root, "/") == 0) {
            rel = argument + 1;
        } else {
            if (strncmp(argument, root, n) != 0 || argument[n] != '/')
                return -FYX_EINVAL;
            rel = argument + n + 1;
        }
    }
    if (!fg_valid_relative(rel))
        return -FYX_EINVAL;
    strcpy(relative, rel);
    return fg_join(root, rel, full);
}

static u32 fg_hash(u32 hash, const void *data, size_t length)
{
    const u8 *bytes = data;
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool fg_valid_blob(const struct fg_repo *repo, const struct fg_blob *blob)
{
    if (strnlen(blob->path, FG_PATH_CAP) >= FG_PATH_CAP ||
        !fg_valid_relative(blob->path) || blob->size > FG_BLOB_CAP ||
        blob->offset > repo->pool_used ||
        blob->size > repo->pool_used - blob->offset)
        return false;
    return fyx_crc32(repo->pool + blob->offset, blob->size) == blob->crc;
}

static int fg_validate(struct fg_repo *repo, const char *root, size_t size)
{
    if (size != sizeof(*repo) || memcmp(repo->magic, "FYXGIT01", 8) != 0 ||
        repo->version != FG_VERSION || repo->commit_count > FG_MAX_COMMITS ||
        repo->index_count > FG_MAX_FILES || repo->pool_used > FG_POOL_CAP ||
        strnlen(repo->root, FG_ROOT_CAP) >= FG_ROOT_CAP ||
        strcmp(repo->root, root) != 0)
        return -FYX_EINVAL;
    u32 saved = repo->checksum;
    repo->checksum = 0;
    u32 actual = fyx_crc32(repo, sizeof(*repo));
    repo->checksum = saved;
    if (saved != actual)
        return -FYX_EIO;
    for (u32 i = 0; i < repo->index_count; i++)
        if (!fg_valid_blob(repo, &repo->index[i]))
            return -FYX_EIO;
    for (u32 i = 0; i < repo->commit_count; i++) {
        struct fg_commit *commit = &repo->commits[i];
        if (commit->count > FG_MAX_FILES || !commit->id ||
            !memchr(commit->message, 0, FG_MESSAGE_CAP))
            return -FYX_EIO;
        for (u32 j = 0; j < commit->count; j++)
            if (!fg_valid_blob(repo, &commit->files[j]))
                return -FYX_EIO;
    }
    bool head_found = repo->head == 0;
    for (u32 i = 0; i < repo->commit_count; i++)
        if (repo->commits[i].id == repo->head)
            head_found = true;
    if (!head_found)
        return -FYX_EIO;
    return FYX_OK;
}

static int fg_load(const char *root, struct fg_repo **out)
{
    *out = NULL;
    char path[VFS_PATH_MAX];
    int rc = fg_repo_path(root, path);
    if (rc != 0)
        return rc;
    void *raw = NULL;
    size_t size = 0;
    rc = vfs_read_file(path, &raw, &size);
    if (rc != 0)
        return rc;
    struct fg_repo *repo = raw;
    rc = fg_validate(repo, root, size);
    if (rc != 0) {
        kfree(repo);
        return rc;
    }
    *out = repo;
    return FYX_OK;
}

static int fg_store(struct fg_repo *repo)
{
    char path[VFS_PATH_MAX];
    int rc = fg_repo_path(repo->root, path);
    if (rc != 0)
        return rc;
    repo->checksum = 0;
    repo->checksum = fyx_crc32(repo, sizeof(*repo));
    return vfs_write_file(path, repo, sizeof(*repo));
}

static struct fg_blob *fg_find(struct fg_blob *files, u32 count,
                               const char *path)
{
    for (u32 i = 0; i < count; i++)
        if (strcmp(files[i].path, path) == 0)
            return &files[i];
    return NULL;
}

static const struct fg_blob *fg_find_const(const struct fg_blob *files,
                                           u32 count, const char *path)
{
    for (u32 i = 0; i < count; i++)
        if (strcmp(files[i].path, path) == 0)
            return &files[i];
    return NULL;
}

static const struct fg_commit *fg_commit_by_id(const struct fg_repo *repo,
                                               u32 id)
{
    if (!id)
        return NULL;
    for (u32 i = 0; i < repo->commit_count; i++)
        if (repo->commits[i].id == id)
            return &repo->commits[i];
    return NULL;
}

static const struct fg_commit *fg_head(const struct fg_repo *repo)
{
    return fg_commit_by_id(repo, repo->head);
}

static const struct fg_blob *fg_head_file(const struct fg_repo *repo,
                                          const char *path)
{
    const struct fg_commit *head = fg_head(repo);
    return head ? fg_find_const(head->files, head->count, path) : NULL;
}

static bool fg_same_blob(const struct fg_blob *a, const struct fg_blob *b)
{
    return a && b && a->size == b->size && a->crc == b->crc;
}

static bool fg_same_tree(const struct fg_blob *a, u32 ac,
                         const struct fg_blob *b, u32 bc)
{
    if (ac != bc)
        return false;
    for (u32 i = 0; i < ac; i++) {
        const struct fg_blob *other = fg_find_const(b, bc, a[i].path);
        if (!fg_same_blob(&a[i], other))
            return false;
    }
    return true;
}

static int fg_read_work(const char *root, const char *relative,
                        void **data, size_t *size)
{
    char path[VFS_PATH_MAX];
    int rc = fg_join(root, relative, path);
    if (rc != 0)
        return rc;
    rc = vfs_read_file(path, data, size);
    if (rc != 0)
        return rc;
    if (*size > FG_BLOB_CAP) {
        kfree(*data);
        *data = NULL;
        *size = 0;
        return -FYX_ERANGE;
    }
    return FYX_OK;
}

static int fg_init(const char *root)
{
    struct vfs_stat st;
    int rc = vfs_stat(root, &st);
    if (rc == -FYX_ENOENT) {
        rc = vfs_mkdir(root);
        if (rc != 0)
            return rc;
    } else if (rc != 0) {
        return rc;
    } else if (st.type != FYXFS_NODE_DIR) {
        return -FYX_ENOTDIR;
    }
    char path[VFS_PATH_MAX];
    rc = fg_repo_path(root, path);
    if (rc != 0)
        return rc;
    rc = vfs_stat(path, &st);
    if (rc == 0) {
        kprintf("[git] repository already initialized at %s\n", root);
        return FYX_OK;
    }
    if (rc != -FYX_ENOENT)
        return rc;
    struct fg_repo *repo = kzalloc(sizeof(*repo));
    if (!repo)
        return -FYX_ENOMEM;
    memcpy(repo->magic, "FYXGIT01", 8);
    repo->version = FG_VERSION;
    strncpy(repo->root, root, sizeof(repo->root) - 1);
    rc = fg_store(repo);
    kfree(repo);
    if (rc == 0)
        kprintf("[git] initialized empty FYXGit repository at %s\n", root);
    return rc;
}

static int fg_add(const char *root, const char *argument)
{
    char relative[FG_PATH_CAP], full[VFS_PATH_MAX];
    int rc = fg_relative(root, argument, relative, full);
    if (rc != 0)
        return rc;
    struct fg_repo *repo;
    rc = fg_load(root, &repo);
    if (rc != 0)
        return rc;
    void *data = NULL;
    size_t size = 0;
    rc = fg_read_work(root, relative, &data, &size);
    if (rc != 0) {
        kfree(repo);
        return rc;
    }
    struct fg_blob *blob = fg_find(repo->index, repo->index_count, relative);
    if (!blob) {
        if (repo->index_count >= FG_MAX_FILES) {
            kfree(data);
            kfree(repo);
            return -FYX_ENOSPC;
        }
        blob = &repo->index[repo->index_count++];
        memset(blob, 0, sizeof(*blob));
        strncpy(blob->path, relative, sizeof(blob->path) - 1);
    }
    if (size > FG_POOL_CAP - repo->pool_used) {
        kfree(data);
        kfree(repo);
        return -FYX_ENOSPC;
    }
    blob->offset = repo->pool_used;
    blob->size = (u32)size;
    blob->crc = fyx_crc32(data, size);
    memcpy(repo->pool + repo->pool_used, data, size);
    repo->pool_used += (u32)size;
    rc = fg_store(repo);
    if (rc == 0)
        kprintf("[git] add %s（%u 字节）\n", relative, (unsigned)size);
    kfree(data);
    kfree(repo);
    return rc;
}

static u32 fg_commit_id(const struct fg_repo *repo,
                        const struct fg_commit *commit)
{
    u32 hash = 2166136261u;
    hash = fg_hash(hash, &commit->parent, sizeof(commit->parent));
    hash = fg_hash(hash, commit->message, sizeof(commit->message));
    hash = fg_hash(hash, &commit->count, sizeof(commit->count));
    for (u32 i = 0; i < commit->count; i++) {
        const struct fg_blob *blob = &commit->files[i];
        hash = fg_hash(hash, blob->path, sizeof(blob->path));
        hash = fg_hash(hash, &blob->size, sizeof(blob->size));
        hash = fg_hash(hash, repo->pool + blob->offset, blob->size);
    }
    return hash ? hash : 1;
}

static int fg_commit(const char *root, int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[1], "-m") != 0)
        return -FYX_EINVAL;
    struct fg_repo *repo;
    int rc = fg_load(root, &repo);
    if (rc != 0)
        return rc;
    const struct fg_commit *head = fg_head(repo);
    if (!repo->index_count ||
        (head && fg_same_tree(repo->index, repo->index_count,
                              head->files, head->count))) {
        kfree(repo);
        kprintf("[git] nothing to commit\n");
        return FYX_OK;
    }
    if (repo->commit_count >= FG_MAX_COMMITS) {
        kfree(repo);
        return -FYX_ENOSPC;
    }
    struct fg_commit *commit = &repo->commits[repo->commit_count];
    memset(commit, 0, sizeof(*commit));
    commit->parent = repo->head;
    commit->count = repo->index_count;
    size_t used = 0;
    for (int i = 2; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (used + n + (i > 2 ? 1 : 0) >= sizeof(commit->message))
            break;
        if (i > 2)
            commit->message[used++] = ' ';
        memcpy(commit->message + used, argv[i], n);
        used += n;
    }
    if (!used)
        strncpy(commit->message, "(no message)", sizeof(commit->message) - 1);
    memcpy(commit->files, repo->index, sizeof(commit->files));
    commit->id = fg_commit_id(repo, commit);
    repo->head = commit->id;
    repo->commit_count++;
    rc = fg_store(repo);
    if (rc == 0)
        kprintf("[git] commit %08x %s（%u 个文件）\n",
                commit->id, commit->message, commit->count);
    kfree(repo);
    return rc;
}

static int fg_log(const char *root)
{
    struct fg_repo *repo;
    int rc = fg_load(root, &repo);
    if (rc != 0)
        return rc;
    if (!repo->head)
        kprintf("[git] no commits\n");
    u32 id = repo->head;
    for (u32 depth = 0; id && depth < FG_MAX_COMMITS; depth++) {
        const struct fg_commit *commit = fg_commit_by_id(repo, id);
        if (!commit) {
            kfree(repo);
            return -FYX_EIO;
        }
        kprintf("[git] %08x parent=%08x %s\n",
                commit->id, commit->parent, commit->message);
        id = commit->parent;
    }
    kfree(repo);
    return FYX_OK;
}

static bool fg_binary(const u8 *data, size_t size)
{
    for (size_t i = 0; i < size; i++)
        if (data[i] == 0)
            return true;
    return false;
}

static u32 fg_lines(const u8 *data, size_t size, struct fg_line *lines,
                    u32 cap, bool *too_many)
{
    u32 count = 0;
    size_t start = 0;
    *too_many = false;
    while (start < size) {
        size_t end = start;
        while (end < size && data[end] != '\n')
            end++;
        if (count >= cap) {
            *too_many = true;
            return count;
        }
        lines[count].data = data + start;
        lines[count].length = end - start;
        count++;
        start = end < size ? end + 1 : end;
    }
    return count;
}

static bool fg_line_equal(const struct fg_line *a, const struct fg_line *b)
{
    return a->length == b->length &&
           (a->length == 0 || memcmp(a->data, b->data, a->length) == 0);
}

static void fg_print_line(char sign, const struct fg_line *line)
{
    console_putc(sign);
    console_write((const char *)line->data, line->length);
    console_putc('\n');
}

static void fg_print_diff(const char *path, const u8 *old_data, size_t old_size,
                          const u8 *new_data, size_t new_size)
{
    kprintf("diff --git a/%s b/%s\n", path, path);
    kprintf("--- a/%s\n+++ b/%s\n", path, path);
    if (fg_binary(old_data, old_size) || fg_binary(new_data, new_size)) {
        kprintf("Binary files differ\n");
        return;
    }
    struct fg_line *old_lines = kmalloc(256 * sizeof(*old_lines));
    struct fg_line *new_lines = kmalloc(256 * sizeof(*new_lines));
    if (!old_lines || !new_lines) {
        kprintf("Diff buffer exhausted\n");
        kfree(old_lines);
        kfree(new_lines);
        return;
    }
    bool old_many, new_many;
    u32 old_count = fg_lines(old_data, old_size, old_lines, 256, &old_many);
    u32 new_count = fg_lines(new_data, new_size, new_lines, 256, &new_many);
    if (old_many || new_many) {
        kprintf("Diff line limit exceeded\n");
        kfree(old_lines);
        kfree(new_lines);
        return;
    }
    u32 prefix = 0;
    while (prefix < old_count && prefix < new_count &&
           fg_line_equal(&old_lines[prefix], &new_lines[prefix]))
        prefix++;
    u32 suffix = 0;
    while (suffix < old_count - prefix && suffix < new_count - prefix &&
           fg_line_equal(&old_lines[old_count - 1 - suffix],
                         &new_lines[new_count - 1 - suffix]))
        suffix++;
    u32 old_changed = old_count - prefix - suffix;
    u32 new_changed = new_count - prefix - suffix;
    kprintf("@@ -%u,%u +%u,%u @@\n", prefix + (old_changed ? 1 : 0),
            old_changed, prefix + (new_changed ? 1 : 0), new_changed);
    for (u32 i = prefix; i < old_count - suffix; i++)
        fg_print_line('-', &old_lines[i]);
    for (u32 i = prefix; i < new_count - suffix; i++)
        fg_print_line('+', &new_lines[i]);
    kfree(old_lines);
    kfree(new_lines);
}

static int fg_diff_one(struct fg_repo *repo, const char *relative, bool cached,
                       bool *different)
{
    const struct fg_commit *head = fg_head(repo);
    const struct fg_blob *old_blob = cached
        ? fg_head_file(repo, relative)
        : fg_find_const(repo->index, repo->index_count, relative);
    const struct fg_blob *new_blob = cached
        ? fg_find_const(repo->index, repo->index_count, relative) : NULL;
    const u8 *old_data = old_blob ? repo->pool + old_blob->offset : NULL;
    size_t old_size = old_blob ? old_blob->size : 0;
    const u8 *new_data = new_blob ? repo->pool + new_blob->offset : NULL;
    size_t new_size = new_blob ? new_blob->size : 0;
    void *work = NULL;
    bool new_present = new_blob != NULL;
    if (!cached) {
        size_t size = 0;
        int rc = fg_read_work(repo->root, relative, &work, &size);
        if (rc == 0) {
            new_data = work;
            new_size = size;
            new_present = true;
        } else if (rc != -FYX_ENOENT) {
            return rc;
        }
    }
    UNUSED(head);
    bool same;
    if (cached) {
        same = (!old_blob && !new_blob) || fg_same_blob(old_blob, new_blob);
    } else if (!old_blob && !new_present) {
        same = true;
    } else if (old_blob && new_present && old_blob->size == new_size &&
               old_blob->crc == fyx_crc32(new_data, new_size)) {
        same = true;
    } else {
        same = false;
    }
    *different = !same;
    if (!same)
        fg_print_diff(relative, old_data, old_size, new_data, new_size);
    kfree(work);
    return FYX_OK;
}

static int fg_collect_paths(struct fg_repo *repo, char paths[][FG_PATH_CAP],
                            u32 *count)
{
    const struct fg_commit *head = fg_head(repo);
    for (u32 pass = 0; pass < 2; pass++) {
        const struct fg_blob *files = pass == 0 ? repo->index
            : head ? head->files : NULL;
        u32 n = pass == 0 ? repo->index_count : head ? head->count : 0;
        for (u32 i = 0; i < n; i++) {
            bool found = false;
            for (u32 j = 0; j < *count; j++)
                if (strcmp(paths[j], files[i].path) == 0)
                    found = true;
            if (!found) {
                if (*count >= FG_MAX_FILES * 2)
                    return -FYX_ENOSPC;
                strncpy(paths[*count], files[i].path, FG_PATH_CAP - 1);
                paths[*count][FG_PATH_CAP - 1] = '\0';
                (*count)++;
            }
        }
    }
    return FYX_OK;
}

static int fg_diff(const char *root, int argc, char **argv)
{
    bool cached = argc > 1 && strcmp(argv[1], "--cached") == 0;
    const char *filter = NULL;
    if (cached && argc > 2)
        filter = argv[2];
    else if (!cached && argc > 1)
        filter = argv[1];
    if (argc > (cached ? 3 : 2))
        return -FYX_EINVAL;
    struct fg_repo *repo;
    int rc = fg_load(root, &repo);
    if (rc != 0)
        return rc;
    char paths[FG_MAX_FILES * 2][FG_PATH_CAP];
    u32 count = 0;
    char filtered[FG_PATH_CAP], full[VFS_PATH_MAX];
    if (filter) {
        rc = fg_relative(root, filter, filtered, full);
        if (rc == 0) {
            strncpy(paths[0], filtered, FG_PATH_CAP - 1);
            paths[0][FG_PATH_CAP - 1] = '\0';
            count = 1;
        }
    } else {
        rc = fg_collect_paths(repo, paths, &count);
    }
    bool any = false;
    for (u32 i = 0; rc == 0 && i < count; i++) {
        bool different = false;
        rc = fg_diff_one(repo, paths[i], cached, &different);
        any |= different;
    }
    if (rc == 0 && !any)
        kprintf("[git] no differences\n");
    kfree(repo);
    return rc;
}

static int fg_status(const char *root)
{
    struct fg_repo *repo;
    int rc = fg_load(root, &repo);
    if (rc != 0)
        return rc;
    const struct fg_commit *head = fg_head(repo);
    bool dirty = false;
    for (u32 pass = 0; pass < 2; pass++) {
        const struct fg_blob *files = pass == 0 ? repo->index
            : head ? head->files : NULL;
        u32 count = pass == 0 ? repo->index_count : head ? head->count : 0;
        for (u32 i = 0; i < count; i++) {
            const struct fg_blob *index = fg_find_const(repo->index,
                                                         repo->index_count,
                                                         files[i].path);
            const struct fg_blob *committed = fg_head_file(repo, files[i].path);
            if (pass == 0 || !index) {
                if (!committed && index) {
                    kprintf("[git] A %s (staged)\n", index->path);
                    dirty = true;
                } else if (committed && !index) {
                    kprintf("[git] D %s (staged)\n", committed->path);
                    dirty = true;
                } else if (committed && index && !fg_same_blob(committed, index)) {
                    kprintf("[git] M %s (staged)\n", index->path);
                    dirty = true;
                }
            }
            if (pass != 0 || !index)
                continue;
            void *work = NULL;
            size_t size = 0;
            int read_rc = fg_read_work(root, index->path, &work, &size);
            if (read_rc == -FYX_ENOENT) {
                kprintf("[git] D %s (worktree)\n", index->path);
                dirty = true;
            } else if (read_rc != 0) {
                kfree(work);
                kfree(repo);
                return read_rc;
            } else if (size != index->size ||
                       fyx_crc32(work, size) != index->crc) {
                kprintf("[git] M %s (worktree)\n", index->path);
                dirty = true;
            }
            kfree(work);
        }
    }
    /* v1 的未跟踪扫描覆盖仓库根的直接文件；跟踪文件可为子目录路径。 */
    for (u32 i = 0;; i++) {
        struct fyxfs_node_info info;
        rc = vfs_getdents(root, i, &info);
        if (rc == -FYX_ENOENT)
            break;
        if (rc != 0) {
            kfree(repo);
            return rc;
        }
        if (info.type != FYXFS_NODE_FILE || strcmp(info.name, FG_REPO_NAME) == 0 ||
            fg_find_const(repo->index, repo->index_count, info.name) ||
            fg_head_file(repo, info.name))
            continue;
        kprintf("[git] ?? %s\n", info.name);
        dirty = true;
    }
    if (!dirty)
        kprintf("[git] clean\n");
    kfree(repo);
    return FYX_OK;
}

static int fg_restore(const char *root, int argc, char **argv)
{
    bool staged = argc > 1 && strcmp(argv[1], "--staged") == 0;
    const char *argument = staged && argc > 2 ? argv[2]
        : !staged && argc > 1 ? argv[1] : NULL;
    if (!argument || argc > (staged ? 3 : 2))
        return -FYX_EINVAL;
    char relative[FG_PATH_CAP], full[VFS_PATH_MAX];
    int rc = fg_relative(root, argument, relative, full);
    if (rc != 0)
        return rc;
    struct fg_repo *repo;
    rc = fg_load(root, &repo);
    if (rc != 0)
        return rc;
    struct fg_blob *index = fg_find(repo->index, repo->index_count, relative);
    const struct fg_blob *committed = fg_head_file(repo, relative);
    if (staged) {
        if (committed) {
            if (!index) {
                if (repo->index_count >= FG_MAX_FILES) {
                    kfree(repo);
                    return -FYX_ENOSPC;
                }
                index = &repo->index[repo->index_count++];
            }
            *index = *committed;
        } else if (index) {
            u32 at = (u32)(index - repo->index);
            for (u32 i = at + 1; i < repo->index_count; i++)
                repo->index[i - 1] = repo->index[i];
            repo->index_count--;
            memset(&repo->index[repo->index_count], 0, sizeof(*index));
        } else {
            kfree(repo);
            return -FYX_ENOENT;
        }
        rc = fg_store(repo);
    } else {
        const struct fg_blob *source = index ? index : committed;
        if (!source) {
            kfree(repo);
            return -FYX_ENOENT;
        }
        rc = vfs_write_file(full, repo->pool + source->offset, source->size);
    }
    if (rc == 0)
        kprintf("[git] restore %s%s\n", relative, staged ? " (index)" : "");
    kfree(repo);
    return rc;
}

static int fg_report_error(int rc)
{
    if (rc == -FYX_ENOENT)
        kprintf("git: 文件或仓库不存在（先执行 git init）\n");
    else if (rc == -FYX_ENOSPC || rc == -FYX_ERANGE)
        kprintf("git: 超出 FYXGit v1 容量限制（%d）\n", rc);
    else if (rc != 0)
        kprintf("git: 操作失败（%d）\n", rc);
    return rc == 0 ? 0 : 1;
}

int fyxgit_command(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("用法: git [-C root] init|add|commit|status|log|diff|restore ...\n");
        return 1;
    }
    const char *root_arg = FG_DEFAULT_ROOT;
    int at = 1;
    if (strcmp(argv[at], "-C") == 0) {
        if (argc < 4)
            return fg_report_error(-FYX_EINVAL);
        root_arg = argv[2];
        at = 3;
    }
    if (at >= argc)
        return fg_report_error(-FYX_EINVAL);
    char root[FG_ROOT_CAP];
    int rc = fg_normalize_root(root_arg, root);
    if (rc != 0)
        return fg_report_error(rc);
    const char *command = argv[at];
    int sub_argc = argc - at;
    char **sub_argv = argv + at;
    if (strcmp(command, "init") == 0) {
        if (sub_argc == 2) {
            rc = fg_normalize_root(sub_argv[1], root);
            if (rc != 0)
                return fg_report_error(rc);
        } else if (sub_argc != 1) {
            return fg_report_error(-FYX_EINVAL);
        }
        rc = fg_init(root);
    } else if (strcmp(command, "add") == 0 && sub_argc == 2) {
        rc = fg_add(root, sub_argv[1]);
    } else if (strcmp(command, "commit") == 0) {
        rc = fg_commit(root, sub_argc, sub_argv);
    } else if (strcmp(command, "status") == 0 && sub_argc == 1) {
        rc = fg_status(root);
    } else if (strcmp(command, "log") == 0 && sub_argc == 1) {
        rc = fg_log(root);
    } else if (strcmp(command, "diff") == 0) {
        rc = fg_diff(root, sub_argc, sub_argv);
    } else if (strcmp(command, "restore") == 0) {
        rc = fg_restore(root, sub_argc, sub_argv);
    } else {
        kprintf("git: 未知或参数错误的子命令\n");
        return 1;
    }
    return fg_report_error(rc);
}
