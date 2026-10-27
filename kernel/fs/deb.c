/*
 * FYXOS — Debian binary package（ar + ustar/gzip）安装器
 *
 * 严格边界：debian-binary 2.0、control.tar[.gz]、data.tar[.gz]、amd64、
 * 普通文件/目录。xz/zstd、依赖求解、maintainer scripts、链接或 pax 扩展
 * 必须报错，不能跳过之后声称安装成功。
 */
#include <fyxos/deb.h>
#include <fyxos/vfs.h>
#include <fyxos/mm.h>
#include <fyxos/lib.h>
#include <fyxos/kernel.h>
#include <fyxos/gzip.h>

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} PACKED;

struct tar_view {
    char path[VFS_PATH_MAX];
    const u8 *data;
    u64 size;
    char type;
};

_Static_assert(sizeof(struct tar_header) == 512, "ustar header must be 512 bytes");

static int parse_number(const char *field, size_t width, u32 base, u64 *out)
{
    if (field[0] & 0x80) /* GNU/base-256 不属于这个严格子集 */
        return -FYX_ENOSYS;
    u64 value = 0;
    size_t i = 0;
    bool any = false;
    while (i < width && (field[i] == ' ' || field[i] == '\t'))
        i++;
    for (; i < width; i++) {
        char c = field[i];
        if (c == 0 || c == ' ')
            break;
        u32 digit;
        if (c >= '0' && c <= '9')
            digit = (u32)(c - '0');
        else
            return -FYX_EINVAL;
        if (digit >= base || value > (~0ull - digit) / base)
            return -FYX_EINVAL;
        value = value * base + digit;
        any = true;
    }
    for (; i < width; i++) {
        if (field[i] != 0 && field[i] != ' ')
            return -FYX_EINVAL;
    }
    if (!any)
        return -FYX_EINVAL;
    *out = value;
    return 0;
}

static bool ar_name_equals(const char field[16], const char *name)
{
    size_t n = strnlen(field, 16);
    while (n && field[n - 1] == ' ')
        n--;
    if (n && field[n - 1] == '/')
        n--;
    size_t want = strlen(name);
    return n == want && memcmp(field, name, want) == 0;
}

static int normalize_tar_path(const struct tar_header *hdr, char out[VFS_PATH_MAX])
{
    char raw[256];
    size_t prefix = strnlen(hdr->prefix, sizeof(hdr->prefix));
    size_t name = strnlen(hdr->name, sizeof(hdr->name));
    if (prefix + name + 2 > sizeof(raw))
        return -FYX_ERANGE;
    size_t n = 0;
    if (prefix) {
        memcpy(raw, hdr->prefix, prefix);
        n = prefix;
        raw[n++] = '/';
    }
    memcpy(raw + n, hdr->name, name);
    n += name;
    raw[n] = 0;

    size_t in = 0, used = 1;
    out[0] = '/';
    out[1] = 0;
    while (in < n) {
        while (in < n && raw[in] == '/')
            in++;
        size_t start = in;
        while (in < n && raw[in] != '/')
            in++;
        size_t len = in - start;
        if (!len || (len == 1 && raw[start] == '.'))
            continue;
        if (len == 2 && raw[start] == '.' && raw[start + 1] == '.')
            return -FYX_EINVAL;
        if (used > 1) {
            if (used + 1 >= VFS_PATH_MAX)
                return -FYX_ERANGE;
            out[used++] = '/';
        }
        if (used + len >= VFS_PATH_MAX)
            return -FYX_ERANGE;
        memcpy(out + used, raw + start, len);
        used += len;
        out[used] = 0;
    }
    return 0;
}

static bool tar_block_is_zero(const u8 *block)
{
    for (size_t i = 0; i < 512; i++)
        if (block[i])
            return false;
    return true;
}

/* 返回 1=一项，0=正常结束，负数=格式/边界错误。 */
static int tar_next(const u8 *tar, size_t size, size_t *offset,
                    struct tar_view *view)
{
    if (*offset > size || size - *offset < 512)
        return -FYX_EINVAL;
    const u8 *block = tar + *offset;
    if (tar_block_is_zero(block)) {
        *offset = size;
        return 0;
    }
    const struct tar_header *hdr = (const struct tar_header *)block;
    if (memcmp(hdr->magic, "ustar", 5) != 0)
        return -FYX_EINVAL;

    u64 expected = 0, file_size = 0;
    int rc = parse_number(hdr->chksum, sizeof(hdr->chksum), 8, &expected);
    if (rc != 0)
        return rc;
    rc = parse_number(hdr->size, sizeof(hdr->size), 8, &file_size);
    if (rc != 0)
        return rc;
    u64 actual = 0;
    for (size_t i = 0; i < 512; i++)
        actual += (i >= offsetof(struct tar_header, chksum) &&
                   i < offsetof(struct tar_header, chksum) + 8) ? 32 : block[i];
    if (actual != expected)
        return -FYX_EIO;

    rc = normalize_tar_path(hdr, view->path);
    if (rc != 0)
        return rc;
    view->type = hdr->typeflag ? hdr->typeflag : '0';
    if (view->type != '0' && view->type != '5')
        return -FYX_ENOSYS; /* 链接、pax、设备等不能静默忽略 */
    if (view->type == '5' && file_size != 0)
        return -FYX_EINVAL;
    if (view->path[1] == 0 && view->type != '5')
        return -FYX_EINVAL;

    size_t data_offset = *offset + 512;
    if (file_size > size - data_offset)
        return -FYX_EINVAL;
    view->data = tar + data_offset;
    view->size = file_size;
    u64 padded = ALIGN_UP(file_size, 512);
    if (padded > size - data_offset)
        return -FYX_EINVAL;
    *offset = data_offset + (size_t)padded;
    return 1;
}

static int control_field(const u8 *text, size_t size, const char *field,
                         char *out, size_t out_size)
{
    size_t field_len = strlen(field), pos = 0;
    while (pos < size) {
        size_t end = pos;
        while (end < size && text[end] != '\n')
            end++;
        if (end - pos >= field_len && memcmp(text + pos, field, field_len) == 0) {
            size_t value = pos + field_len;
            while (value < end && (text[value] == ' ' || text[value] == '\t'))
                value++;
            size_t value_end = end;
            if (value_end > value && text[value_end - 1] == '\r')
                value_end--;
            while (value_end > value && text[value_end - 1] == ' ')
                value_end--;
            size_t len = value_end - value;
            if (len >= out_size)
                return -FYX_ERANGE;
            memcpy(out, text + value, len);
            out[len] = 0;
            return 1;
        }
        pos = end < size ? end + 1 : end;
    }
    return 0;
}

struct deb_control {
    char package[32];
    char version[32];
};

static int validate_control(const u8 *tar, size_t size, struct deb_control *info)
{
    size_t offset = 0;
    const u8 *control = NULL;
    size_t control_size = 0;
    for (;;) {
        struct tar_view view;
        int rc = tar_next(tar, size, &offset, &view);
        if (rc < 0)
            return rc;
        if (rc == 0)
            break;
        if (view.type == '5')
            continue;
        if (strcmp(view.path, "/control") != 0 || control)
            return -FYX_ENOSYS; /* conffiles/maintainer scripts 尚未实现 */
        control = view.data;
        control_size = (size_t)view.size;
    }
    if (!control || control_size > 4096)
        return -FYX_EINVAL;

    char architecture[24], depends[256];
    int rc = control_field(control, control_size, "Package:",
                           info->package, sizeof(info->package));
    if (rc != 1 || !info->package[0])
        return rc < 0 ? rc : -FYX_EINVAL;
    rc = control_field(control, control_size, "Version:",
                       info->version, sizeof(info->version));
    if (rc < 0)
        return rc;
    if (rc == 0)
        strncpy(info->version, "unknown", sizeof(info->version) - 1);
    rc = control_field(control, control_size, "Architecture:",
                       architecture, sizeof(architecture));
    if (rc != 1 || strcmp(architecture, "amd64") != 0)
        return rc < 0 ? rc : -FYX_EINVAL;
    rc = control_field(control, control_size, "Depends:",
                       depends, sizeof(depends));
    if (rc < 0)
        return rc;
    if (rc == 1 && depends[0])
        return -FYX_ENOSYS; /* 没有依赖求解器，不假装依赖已满足 */
    rc = control_field(control, control_size, "Pre-Depends:",
                       depends, sizeof(depends));
    if (rc < 0)
        return rc;
    if (rc == 1 && depends[0])
        return -FYX_ENOSYS;
    return 0;
}

static int process_data_tar(const u8 *tar, size_t size, bool extract,
                            u32 *installed_files, u64 *installed_bytes)
{
    size_t offset = 0;
    u32 files = 0;
    u64 bytes = 0;
    for (;;) {
        struct tar_view view;
        int rc = tar_next(tar, size, &offset, &view);
        if (rc < 0)
            return rc;
        if (rc == 0)
            break;
        if (!extract) {
            if (view.type == '0')
                files++;
            continue;
        }

        if (view.path[1] == 0)
            continue; /* ./ 根目录 */
        if (view.type == '5') {
            struct vfs_stat st;
            if (vfs_stat(view.path, &st) == 0) {
                if (st.type != FYXFS_NODE_DIR)
                    return -FYX_EEXIST;
            } else {
                rc = vfs_mkdir(view.path);
                if (rc != 0)
                    return rc;
            }
            continue;
        }
        rc = vfs_write_file(view.path, view.data, (size_t)view.size);
        if (rc != 0)
            return rc;
        files++;
        bytes += view.size;
    }
    if (!files)
        return -FYX_EINVAL;
    if (installed_files)
        *installed_files = files;
    if (installed_bytes)
        *installed_bytes = bytes;
    return 0;
}

int deb_install_image(const void *image, size_t size, const char *source_name)
{
    const u8 *deb = (const u8 *)image;
    if (!deb || size < 8 || memcmp(deb, "!<arch>\n", 8) != 0)
        return -FYX_EINVAL;

    const u8 *control_tar = NULL, *data_tar = NULL;
    size_t control_size = 0, data_size = 0;
    void *control_alloc = NULL, *data_alloc = NULL;
    bool saw_version = false, saw_control = false, saw_data = false;
    bool control_gzip = false, data_gzip = false;
    size_t offset = 8;
    while (offset < size) {
        if (size - offset < 60)
            return -FYX_EINVAL;
        const u8 *hdr = deb + offset;
        if (hdr[58] != '`' || hdr[59] != '\n')
            return -FYX_EINVAL;
        u64 member_size = 0;
        int rc = parse_number((const char *)hdr + 48, 10, 10, &member_size);
        if (rc != 0)
            return rc;
        offset += 60;
        if (member_size > size - offset)
            return -FYX_EINVAL;
        const u8 *data = deb + offset;

        if (ar_name_equals((const char *)hdr, "debian-binary")) {
            if (saw_version || member_size != 4 || memcmp(data, "2.0\n", 4) != 0)
                return -FYX_EINVAL;
            saw_version = true;
        } else if (ar_name_equals((const char *)hdr, "control.tar") ||
                   ar_name_equals((const char *)hdr, "control.tar.gz")) {
            if (saw_control)
                return -FYX_EINVAL;
            saw_control = true;
            control_gzip = ar_name_equals((const char *)hdr, "control.tar.gz");
            control_tar = data;
            control_size = (size_t)member_size;
        } else if (ar_name_equals((const char *)hdr, "data.tar") ||
                   ar_name_equals((const char *)hdr, "data.tar.gz")) {
            if (saw_data)
                return -FYX_EINVAL;
            saw_data = true;
            data_gzip = ar_name_equals((const char *)hdr, "data.tar.gz");
            data_tar = data;
            data_size = (size_t)member_size;
        } else if (ar_name_equals((const char *)hdr, "control.tar.xz") ||
                   ar_name_equals((const char *)hdr, "control.tar.zst") ||
                   ar_name_equals((const char *)hdr, "data.tar.xz") ||
                   ar_name_equals((const char *)hdr, "data.tar.zst")) {
            return -FYX_ENOSYS;
        } else {
            return -FYX_ENOSYS; /* _gpgorigin 等额外成员尚未处理 */
        }
        offset += (size_t)member_size;
        if (member_size & 1) {
            if (offset >= size || deb[offset] != '\n')
                return -FYX_EINVAL;
            offset++;
        }
    }
    if (!saw_version || !saw_control || !saw_data)
        return -FYX_EINVAL;

    int rc = 0;
    if (control_gzip) {
        rc = gzip_decompress(control_tar, control_size, &control_alloc,
                             &control_size, 16u * 1024u * 1024u);
        control_tar = control_alloc;
    }
    if (!rc && data_gzip) {
        rc = gzip_decompress(data_tar, data_size, &data_alloc,
                             &data_size, 16u * 1024u * 1024u);
        data_tar = data_alloc;
    }
    if (rc) {
        kprintf("[deb] gzip 解压失败：%s（错误 %d）\n", source_name, rc);
        goto out;
    }

    struct deb_control info;
    memset(&info, 0, sizeof(info));
    rc = validate_control(control_tar, control_size, &info);
    if (rc != 0) {
        kprintf("[deb] control 校验失败：%s（错误 %d）\n", source_name, rc);
        goto out;
    }
    /* 第一遍完整验证数据 tar，第二遍才开始写文件。 */
    rc = process_data_tar(data_tar, data_size, false, NULL, NULL);
    if (rc != 0) {
        kprintf("[deb] data.tar 校验失败：%s（错误 %d）\n", source_name, rc);
        goto out;
    }

    u32 files = 0;
    u64 bytes = 0;
    rc = process_data_tar(data_tar, data_size, true, &files, &bytes);
    if (rc != 0) {
        kprintf("[deb] 解包失败：%s（错误 %d；可能已有部分文件）\n",
                source_name, rc);
        goto out;
    }
    const char *format = control_gzip || data_gzip ? "gzip" : "plain";
    kprintf("[deb] 已安装 %s %s：%u 个文件，%llu 字节（amd64，%s）\n",
            info.package, info.version, files, (unsigned long long)bytes,
            control_gzip || data_gzip ? "gzip" : "未压缩");
    kprintf("[deb] INSTALL_OK package=%s version=%s files=%u bytes=%llu arch=amd64 format=%s\n",
            info.package, info.version, files, (unsigned long long)bytes, format);
out:
    if (control_alloc)
        kfree(control_alloc);
    if (data_alloc)
        kfree(data_alloc);
    return rc;
}

int deb_install(const char *package_path)
{
    if (!package_path)
        return -FYX_EINVAL;
    void *image = NULL;
    size_t size = 0;
    int rc = vfs_read_file(package_path, &image, &size);
    if (rc != 0)
        return rc;
    rc = deb_install_image(image, size, package_path);
    kfree(image);
    return rc;
}
