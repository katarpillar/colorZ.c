#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include <setjmp.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <weechat-plugin.h>
WEECHAT_PLUGIN_NAME("colorZ");
WEECHAT_PLUGIN_DESCRIPTION("Un peu de couleur dans vos buffers");
WEECHAT_PLUGIN_AUTHOR("OpodeGaster");
WEECHAT_PLUGIN_VERSION("1.1");
WEECHAT_PLUGIN_LICENSE("GPL3");
struct t_weechat_plugin *weechat_plugin = NULL;
#define NICK_CACHE_MAX 500
#define CHANNEL_CACHE_MAX 300
static char log_file_path[PATH_MAX] = "";
#define DEBUG_LOG_MAX_BYTES (5 * 1024 * 1024)
static char *bold_on  = NULL;
static char *bold_off = NULL;
static char *ital_on  = NULL;
static char *ital_off = NULL;
static char *underline_on  = NULL;
static char *underline_off = NULL;
static struct t_hashtable *nick_color_cache = NULL;
static struct t_hashtable *channel_nicks_cache = NULL;
static int current_lid = 0;
static long line_counter = 0;
static jmp_buf oom_jmp;
static int oom_guard_active = 0;
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} dstr_t;
static void *
xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p)
    {
        weechat_printf(NULL, "colorZ: échec d'allocation mémoire (%zu octets)", n);
        if (oom_guard_active)
            longjmp(oom_jmp, 1);
        abort();
    }
    return p;
}
static void *
xrealloc(void *ptr, size_t n)
{
    void *p = realloc(ptr, n);
    if (!p)
    {
        weechat_printf(NULL, "colorZ: échec de réallocation mémoire (%zu octets)", n);
        if (oom_guard_active)
            longjmp(oom_jmp, 1);
        abort();
    }
    return p;
}
static char *
xstrdup(const char *s)
{
    char *p = strdup(s);
    if (!p)
    {
        weechat_printf(NULL, "colorZ: échec de duplication de chaîne");
        if (oom_guard_active)
            longjmp(oom_jmp, 1);
        abort();
    }
    return p;
}
static void
dstr_init(dstr_t *s)
{
    s->cap = 256;
    s->len = 0;
    s->data = xmalloc(s->cap);
    s->data[0] = '\0';
}
static void
dstr_ensure(dstr_t *s, size_t add)
{
    if (s->len + add + 1 > s->cap)
    {
        while (s->len + add + 1 > s->cap)
            s->cap *= 2;
        s->data = xrealloc(s->data, s->cap);
    }
}
static void
dstr_append_n(dstr_t *s, const char *text, size_t n)
{
    if (!text || n == 0)
        return;
    dstr_ensure(s, n);
    memcpy(s->data + s->len, text, n);
    s->len += n;
    s->data[s->len] = '\0';
}
static void
dstr_append(dstr_t *s, const char *text)
{
    if (text)
        dstr_append_n(s, text, strlen(text));
}
static void
dstr_append_char(dstr_t *s, char c)
{
    dstr_append_n(s, &c, 1);
}
typedef struct {
    int nicks;
    int nicks_bold;
    int nicks_italic;
    int lines;
    int hostmask_nicks;
    int hostmask_nicks_bold;
    int hostmask_nicks_italic;
    int events;
    int events_bold;
    int events_italic;
    int links;
    int links_bold;
    int links_italic;
    int links_underline;
    int input_bar;
    int channel;
    int channel_bold;
    int channel_italic;
    int debug;
} options_cache_t;
static options_cache_t g_opts;
static int
debug_level(void)
{
    return g_opts.debug;
}
static void
debugf(int lvl, const char *fmt, ...)
{
    if (debug_level() < lvl)
        return;
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    char prefix[64];
    if (current_lid)
        snprintf(prefix, sizeof(prefix), "[L%d] ", current_lid);
    else
        prefix[0] = '\0';
    weechat_printf(NULL, "colorZ: %s%s", prefix, msg);
    if (log_file_path[0])
    {
        struct stat st;
        int need_truncate = (stat(log_file_path, &st) == 0
                              && st.st_size > DEBUG_LOG_MAX_BYTES);
        int flags = O_CREAT | O_WRONLY | O_NOFOLLOW
                    | (need_truncate ? O_TRUNC : O_APPEND);
        int fd = open(log_file_path, flags, 0600);
        if (fd >= 0)
        {
            FILE *fh = fdopen(fd, "a");
            if (fh)
            {
                time_t t = time(NULL);
                struct tm *tmv = localtime(&t);
                fprintf(fh, "[%02d:%02d:%02d] %s%s\n",
                        tmv->tm_hour, tmv->tm_min, tmv->tm_sec, prefix, msg);
                fclose(fh);
            }
            else
                close(fd);
        }
    }
}
static void
debug_hex(int lvl, const char *label, const char *s)
{
    if (debug_level() < lvl)
        return;
    if (!s)
    {
        debugf(lvl, "%s: (NULL)", label);
        return;
    }
    dstr_t h;
    dstr_init(&h);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        char buf[8];
        if (*p >= 32 && *p < 127)
            snprintf(buf, sizeof(buf), "%c", *p);
        else
            snprintf(buf, sizeof(buf), "\\x%02X", *p);
        dstr_append(&h, buf);
    }
    debugf(lvl, "%s (len=%zu): %s", label, strlen(s), h.data);
    free(h.data);
}
static int
is_word_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '[';
}
static int
is_at_token(const char *text, int len, int pos)
{
    int start = pos;
    while (start > 0)
    {
        char c = text[start - 1];
        if (c == ' ' || c == '\t' || c == '(' || c == ')')
            break;
        start--;
    }
    (void)len;
    for (int i = start; i < pos; i++)
        if (text[i] == '@')
            return 1;
    return 0;
}
static struct t_gui_buffer *
resolve_buffer(const char *buffer_ptr_str)
{
    void *ptr = NULL;
    if (!buffer_ptr_str || !buffer_ptr_str[0])
        return NULL;
    if (sscanf(buffer_ptr_str, "%p", &ptr) != 1)
        return NULL;
    return (struct t_gui_buffer *)ptr;
}
static int
match_control_seq(const char *s, int len, int pos)
{
    unsigned char c = (unsigned char)s[pos];
    if (c != 0x19 && c != 0x1A && c != 0x1B && c != 0x1C)
        return 0;
    if (c == 0x1C)
        return 1;
    if (c == 0x1A || c == 0x1B)
        return (pos + 1 < len) ? 2 : 1;
    int j = pos + 1;
    if (j < len && s[j] == 'F')
    {
        j++;
        if (j < len && s[j] == '@')
        {
            j++;
            int digits = 0;
            while (j < len && isdigit((unsigned char)s[j]) && digits < 5)
            {
                j++;
                digits++;
            }
            return (digits >= 1) ? (j - pos) : (j - pos);
        }
        else
        {
            int digits = 0;
            while (j < len && isdigit((unsigned char)s[j]) && digits < 3)
            {
                j++;
                digits++;
            }
            if (digits < 2)
                return 1;
            if (j < len && (unsigned char)s[j] == 0x19)
            {
                int k = j + 1, d2 = 0;
                while (k < len && isdigit((unsigned char)s[k]) && d2 < 3)
                {
                    k++;
                    d2++;
                }
                if (d2 >= 2)
                    return k - pos;
            }
            return j - pos;
        }
    }
    else
    {
        int digits = 0;
        while (j < len && isdigit((unsigned char)s[j]) && digits < 2)
        {
            j++;
            digits++;
        }
        if (digits < 2)
            return 1;
        if (j < len && (unsigned char)s[j] == 0x19)
        {
            int k = j + 1, d2 = 0;
            while (k < len && isdigit((unsigned char)s[k]) && d2 < 2)
            {
                k++;
                d2++;
            }
            if (d2 == 2)
                return k - pos;
        }
        return j - pos;
    }
}
static char *nick_cache_order[NICK_CACHE_MAX];
static int nick_cache_order_count = 0;
static int
nick_cache_find_index(const char *key)
{
    for (int i = 0; i < nick_cache_order_count; i++)
        if (strcmp(nick_cache_order[i], key) == 0)
            return i;
    return -1;
}
static void
nick_cache_touch(const char *key)
{
    int idx = nick_cache_find_index(key);
    if (idx < 0)
        return;
    char *entry = nick_cache_order[idx];
    for (int i = idx; i < nick_cache_order_count - 1; i++)
        nick_cache_order[i] = nick_cache_order[i + 1];
    nick_cache_order[nick_cache_order_count - 1] = entry;
}
static void
nick_cache_untrack_all(void)
{
    for (int i = 0; i < nick_cache_order_count; i++)
        free(nick_cache_order[i]);
    nick_cache_order_count = 0;
}
static void
nick_cache_track(const char *key)
{
    if (nick_cache_find_index(key) >= 0)
        return;
    if (nick_cache_order_count >= NICK_CACHE_MAX)
    {
        char *oldest = nick_cache_order[0];
        debugf(2, "Cache couleurs nicks : éviction LRU de '%s' (limite %d atteinte)",
               oldest, NICK_CACHE_MAX);
        weechat_hashtable_remove(nick_color_cache, oldest);
        free(oldest);
        for (int i = 0; i < nick_cache_order_count - 1; i++)
            nick_cache_order[i] = nick_cache_order[i + 1];
        nick_cache_order_count--;
    }
    nick_cache_order[nick_cache_order_count++] = xstrdup(key);
}
static const char *
get_nick_color_code(const char *nick)
{
    const char *cached = weechat_hashtable_get(nick_color_cache, nick);
    if (cached)
    {
        nick_cache_touch(nick);
        return cached;
    }
    const char *color = weechat_info_get("nick_color", nick);
    if (!color || color[0] != 0x19)
        color = weechat_color("default");
    nick_cache_track(nick);
    weechat_hashtable_set(nick_color_cache, nick, color);
    return weechat_hashtable_get(nick_color_cache, nick);
}
static void
free_nick_list(char **list, int n)
{
    for (int i = 0; i < n; i++)
        free(list[i]);
    free(list);
}
static void
add_nick_to_list(char ***list, int *count, int *cap, const char *nick)
{
    if (!nick || !nick[0])
        return;
    for (int i = 0; i < *count; i++)
        if (strcmp((*list)[i], nick) == 0)
            return;
    if (*count >= *cap)
    {
        *cap *= 2;
        *list = xrealloc(*list, sizeof(char *) * (*cap));
    }
    (*list)[*count] = xstrdup(nick);
    (*count)++;
}
static char *channel_cache_order[CHANNEL_CACHE_MAX];
static int channel_cache_order_count = 0;
static int
channel_cache_find_index(const char *key)
{
    for (int i = 0; i < channel_cache_order_count; i++)
        if (strcmp(channel_cache_order[i], key) == 0)
            return i;
    return -1;
}
static void
channel_cache_touch(const char *key)
{
    int idx = channel_cache_find_index(key);
    if (idx < 0)
        return;
    char *entry = channel_cache_order[idx];
    for (int i = idx; i < channel_cache_order_count - 1; i++)
        channel_cache_order[i] = channel_cache_order[i + 1];
    channel_cache_order[channel_cache_order_count - 1] = entry;
}
static void
channel_cache_untrack(const char *key)
{
    int idx = channel_cache_find_index(key);
    if (idx < 0)
        return;
    free(channel_cache_order[idx]);
    for (int i = idx; i < channel_cache_order_count - 1; i++)
        channel_cache_order[i] = channel_cache_order[i + 1];
    channel_cache_order_count--;
}
static void
channel_cache_untrack_all(void)
{
    for (int i = 0; i < channel_cache_order_count; i++)
        free(channel_cache_order[i]);
    channel_cache_order_count = 0;
}
static void
channel_cache_track(const char *key)
{
    if (channel_cache_find_index(key) >= 0)
        return;
    if (channel_cache_order_count >= CHANNEL_CACHE_MAX)
    {
        char *oldest = channel_cache_order[0];
        debugf(2, "Cache nicklists : éviction LRU de '%s' (limite %d atteinte)",
               oldest, CHANNEL_CACHE_MAX);
        weechat_hashtable_remove(channel_nicks_cache, oldest);
        free(oldest);
        for (int i = 0; i < channel_cache_order_count - 1; i++)
            channel_cache_order[i] = channel_cache_order[i + 1];
        channel_cache_order_count--;
    }
    channel_cache_order[channel_cache_order_count++] = xstrdup(key);
}
static void
get_other_nicks(const char *buffer_ptr, const char *sender, char ***out, int *n)
{
    *out = NULL;
    *n = 0;
    struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
    if (!buffer)
        return;
    const char *server  = weechat_buffer_get_string(buffer, "localvar_server");
    const char *channel = weechat_buffer_get_string(buffer, "localvar_channel");
    if (!server || !channel || !server[0] || !channel[0])
        return;
    char cache_key[512];
    snprintf(cache_key, sizeof(cache_key), "%s,%s", server, channel);
    const char *cached = weechat_hashtable_get(channel_nicks_cache, cache_key);
    if (cached)
    {
        channel_cache_touch(cache_key);
    }
    else
    {
        dstr_t joined;
        dstr_init(&joined);
        struct t_gui_nick_group *group = NULL;
        struct t_gui_nick *nick = NULL;
        int first = 1;
        weechat_nicklist_get_next_item(buffer, &group, &nick);
        while (group || nick)
        {
            if (nick)
            {
                const char *nm = weechat_nicklist_nick_get_string(buffer, nick, "name");
                if (nm && nm[0])
                {
                    if (!first)
                        dstr_append_char(&joined, '\x01');
                    dstr_append(&joined, nm);
                    first = 0;
                }
            }
            weechat_nicklist_get_next_item(buffer, &group, &nick);
        }
        if (joined.len > 0)
        {
            channel_cache_track(cache_key);
            weechat_hashtable_set(channel_nicks_cache, cache_key, joined.data);
            debugf(3, "cache nicks construit pour %s (%d octets)", cache_key, (int)joined.len);
            cached = weechat_hashtable_get(channel_nicks_cache, cache_key);
        }
        else
        {
            debugf(3, "nicklist du buffer vide pour %s, pas de mise en cache", cache_key);
        }
        free(joined.data);
    }
    if (!cached)
        return;
    char *copy = xstrdup(cached);
    int capacity = 32;
    char **list = xmalloc(sizeof(char *) * capacity);
    int count = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(copy, "\x01", &saveptr);
    while (tok)
    {
        if (!sender || !sender[0] || strcmp(tok, sender) != 0)
        {
            if (count >= capacity)
            {
                capacity *= 2;
                list = xrealloc(list, sizeof(char *) * capacity);
            }
            list[count++] = xstrdup(tok);
        }
        tok = strtok_r(NULL, "\x01", &saveptr);
    }
    free(copy);
    *out = list;
    *n = count;
}
static char last_333_channel[128] = "";
static char last_333_setter[128] = "";
static int
irc_in_333_signal_cb(const void *pointer, void *data, const char *signal,
                      const char *type_data, void *signal_data)
{
    (void)pointer; (void)data; (void)signal; (void)type_data;
    const char *line = (const char *)signal_data;
    if (!line)
        return WEECHAT_RC_OK;
    if (line[0] == '@')
    {
        const char *space = strchr(line, ' ');
        if (space)
            line = space + 1;
    }
    char *copy = xstrdup(line);
    char *saveptr = NULL;
    char *tok = strtok_r(copy, " ", &saveptr);
    int field = 0;
    char channel[128] = "";
    char setter[128] = "";
    while (tok)
    {
        field++;
        if (field == 4)
            snprintf(channel, sizeof(channel), "%s", tok);
        else if (field == 5)
        {
            char *bang = strchr(tok, '!');
            if (bang)
                *bang = '\0';
            snprintf(setter, sizeof(setter), "%s", tok);
            break;
        }
        tok = strtok_r(NULL, " ", &saveptr);
    }
    free(copy);
    if (channel[0] && setter[0])
    {
        snprintf(last_333_channel, sizeof(last_333_channel), "%s", channel);
        snprintf(last_333_setter, sizeof(last_333_setter), "%s", setter);
        debugf(2, "irc_in_333 : canal=%s, auteur réel du topic=%s",
               channel, setter);
    }
    return WEECHAT_RC_OK;
}
static void
get_nicks_with_author(const char *buffer_ptr, const char *sender,
                      const char *extra_author, char ***out, int *n)
{
    char **others;
    int n_others;
    get_other_nicks(buffer_ptr, sender, &others, &n_others);
    int capacity = n_others + 2;
    char **all = xmalloc(sizeof(char *) * capacity);
    int count = 0;
    for (int i = 0; i < n_others; i++)
        all[count++] = others[i];
    free(others);
    if (sender && sender[0])
        add_nick_to_list(&all, &count, &capacity, sender);
    if (extra_author && extra_author[0])
        add_nick_to_list(&all, &count, &capacity, extra_author);
    *out = all;
    *n = count;
}
static void
invalidate_channel_cache_for(const char *buffer_ptr)
{
    struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
    if (!buffer)
        return;
    const char *server  = weechat_buffer_get_string(buffer, "localvar_server");
    const char *channel = weechat_buffer_get_string(buffer, "localvar_channel");
    if (!server || !channel)
        return;
    char key[512];
    snprintf(key, sizeof(key), "%s,%s", server, channel);
    weechat_hashtable_remove(channel_nicks_cache, key);
    channel_cache_untrack(key);
}
static void
add_nick_to_cache(const char *buffer_ptr, const char *nick)
{
    struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
    if (!buffer)
        return;
    const char *server  = weechat_buffer_get_string(buffer, "localvar_server");
    const char *channel = weechat_buffer_get_string(buffer, "localvar_channel");
    if (!server || !channel)
        return;
    char key[512];
    snprintf(key, sizeof(key), "%s,%s", server, channel);
    const char *cached = weechat_hashtable_get(channel_nicks_cache, key);
    if (!cached)
        return;
    char *copy = xstrdup(cached);
    int already = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(copy, "\x01", &saveptr);
    while (tok)
    {
        if (strcmp(tok, nick) == 0) { already = 1; break; }
        tok = strtok_r(NULL, "\x01", &saveptr);
    }
    free(copy);
    if (already)
        return;
    dstr_t updated;
    dstr_init(&updated);
    dstr_append(&updated, cached);
    if (updated.len > 0)
        dstr_append_char(&updated, '\x01');
    dstr_append(&updated, nick);
    weechat_hashtable_set(channel_nicks_cache, key, updated.data);
    debugf(2, "cache nicks: ajout de '%s' pour %s", nick, key);
    free(updated.data);
}
static int
nicklist_changed_cb(const void *pointer, void *data, const char *signal,
                     const char *type_data, void *signal_data)
{
    (void)pointer; (void)data; (void)type_data; (void)signal_data;
    weechat_hashtable_remove_all(channel_nicks_cache);
    channel_cache_untrack_all();
    debugf(2, "Cache nicks global invalide (signal: %s)", signal);
    return WEECHAT_RC_OK;
}
static int
is_option_enabled(const char *option_name)
{
    const char *v = weechat_config_get_plugin(option_name);
    if (!v)
        return 0;
    return (strcmp(v, "on") == 0);
}
static void
refresh_options_cache(void)
{
    g_opts.nicks = is_option_enabled("nicks");
    g_opts.nicks_bold = is_option_enabled("nicks_bold");
    g_opts.nicks_italic = is_option_enabled("nicks_italic");
    g_opts.lines = is_option_enabled("lines");
    g_opts.hostmask_nicks = is_option_enabled("hostmask_nicks");
    g_opts.hostmask_nicks_bold = is_option_enabled("hostmask_nicks_bold");
    g_opts.hostmask_nicks_italic = is_option_enabled("hostmask_nicks_italic");
    g_opts.events = is_option_enabled("events");
    g_opts.events_bold = is_option_enabled("events_bold");
    g_opts.events_italic = is_option_enabled("events_italic");
    g_opts.links = is_option_enabled("links");
    g_opts.links_bold = is_option_enabled("links_bold");
    g_opts.links_italic = is_option_enabled("links_italic");
    g_opts.links_underline = is_option_enabled("links_underline");
    g_opts.input_bar = is_option_enabled("input_bar");
    g_opts.channel = is_option_enabled("channel");
    g_opts.channel_bold = is_option_enabled("channel_bold");
    g_opts.channel_italic = is_option_enabled("channel_italic");
    const char *v = weechat_config_get_plugin("debug");
    g_opts.debug = v ? atoi(v) : 0;
}
static int
config_changed_cb(const void *pointer, void *data,
                   const char *option, const char *value)
{
    (void)pointer; (void)data; (void)option; (void)value;
    refresh_options_cache();
    return WEECHAT_RC_OK;
}
static int
nicks_bold_enabled(void)
{
    return g_opts.nicks_bold;
}
static int
nicks_italic_enabled(void)
{
    return g_opts.nicks_italic;
}
static int
nicks_enabled(void)
{
    return g_opts.nicks;
}
static int
lines_enabled(void)
{
    return g_opts.lines;
}
static int
hostmask_nicks_enabled(void)
{
    return g_opts.hostmask_nicks;
}
static int
hostmask_nicks_bold_enabled(void)
{
    return g_opts.hostmask_nicks_bold;
}
static int
hostmask_nicks_italic_enabled(void)
{
    return g_opts.hostmask_nicks_italic;
}
static int
events_enabled(void)
{
    return g_opts.events;
}
static int
events_bold_enabled(void)
{
    return g_opts.events_bold;
}
static int
events_italic_enabled(void)
{
    return g_opts.events_italic;
}
static int
links_bold_enabled(void)
{
    return g_opts.links_bold;
}
static int
links_italic_enabled(void)
{
    return g_opts.links_italic;
}
static int
links_underline_enabled(void)
{
    return g_opts.links_underline;
}
static int
links_enabled(void)
{
    return g_opts.links;
}
static int
input_bar_enabled(void)
{
    return g_opts.input_bar;
}
static int
channel_enabled(void)
{
    return g_opts.channel;
}
struct markers_s;
typedef struct markers_s markers_t;
static const char *last_color_before(const char *clean, int pos, markers_t *markers);
static void emit_marked(dstr_t *out, markers_t *markers, const char *code);
static void
emit_color(dstr_t *out, markers_t *markers, const char *code)
{
    if (markers)
        emit_marked(out, markers, code);
    else
        dstr_append(out, code);
}
static void
append_formatted_nick(dstr_t *out, markers_t *markers, const char *nick, const char *color)
{
    int use_bold = nicks_bold_enabled();
    int use_italic = nicks_italic_enabled();
    emit_color(out, markers, color);
    if (use_bold)
        emit_color(out, markers, bold_on);
    if (use_italic)
        emit_color(out, markers, ital_on);
    dstr_append(out, nick);
    if (use_bold)
        emit_color(out, markers, bold_off);
    if (use_italic)
        emit_color(out, markers, ital_off);
}
static int
cmp_nick_length_desc(const void *a, const void *b)
{
    const char *na = *(char * const *)a;
    const char *nb = *(char * const *)b;
    size_t la = strlen(na);
    size_t lb = strlen(nb);
    if (la != lb)
        return (la < lb) ? 1 : -1;
    return 0;
}
static void
sort_nicks_by_length_desc(char **nicks, int n)
{
    if (n > 1)
        qsort(nicks, (size_t)n, sizeof(char *), cmp_nick_length_desc);
}
static int
is_url(const char *text, int len)
{
    if (len < 8)
        return 0;
    const char *protocols[] = {"http://", "https://"};
    int n_protocols = sizeof(protocols) / sizeof(protocols[0]);
    for (int i = 0; i < n_protocols; i++)
    {
        int plen = strlen(protocols[i]);
        if (len >= plen && strncmp(text, protocols[i], plen) == 0)
            return 1;
    }
    return 0;
}
static int
url_span_len(const char *text, int len, int pos)
{
    int start = pos;
    pos++;
    int paren_depth = 0;
    while (pos < len)
    {
        int seqlen = match_control_seq(text, len, pos);
        if (seqlen > 0)
            break;
        char c = text[pos];
        if (c == '(')
        {
            /* Parenthese ouvrante appartenant potentiellement a l'URL
             * elle-meme (ex: nom de fichier "...(3).pdf") : on ne coupe
             * pas dessus, on se contente de suivre la profondeur pour
             * savoir si une ')' plus loin lui correspond. */
            paren_depth++;
            pos++;
            continue;
        }
        if (c == ')')
        {
            if (paren_depth > 0)
            {
                /* Cette parenthese fermante correspond a une ouvrante
                 * rencontree dans l'URL : elle en fait partie. */
                paren_depth--;
                pos++;
                continue;
            }
            /* Parenthese fermante non appariee : c'est celle qui entoure
             * l'URL dans le texte (ex: "(voir https://exemple.com)"). */
            break;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == '"' || c == ']' || c == '}' || c == '>' ||
            c == '\x0E' || c == '\x0F')
            break;
        if ((c == '.' || c == ',') && pos + 1 < len &&
            (text[pos + 1] == ' ' || text[pos + 1] == '\t' || text[pos + 1] == '\n' ||
             (text[pos + 1] == ')' && paren_depth == 0) || text[pos + 1] == '"'))
            break;
        pos++;
    }
    return pos - start;
}
static char *
colorize_links_in_text(const char *text, const char *link_color_name,
                        const char *restore_color, markers_t *markers)
{
    if (!text || !text[0])
        return xstrdup(text ? text : "");
    if (!links_enabled())
        return xstrdup(text);
    int len = strlen(text);
    dstr_t out;
    dstr_init(&out);
    int pos = 0;
    int in_url = 0;
    int url_paren_depth = 0;
    int links_bold = links_bold_enabled();
    int links_italic = links_italic_enabled();
    int links_underline = links_underline_enabled();
    const char *default_color = weechat_color("default");
    const char *link_color = weechat_color(link_color_name);
    const char *restore = restore_color ? restore_color : default_color;
    char ambient_buf[32];
    int ambient_len = 0;
    const char *url_ambient = NULL;
    emit_color(&out, markers, restore);
    while (pos < len)
    {
        int seqlen = match_control_seq(text, len, pos);
        if (seqlen > 0)
        {
            const char *cur_restore = markers
                ? (url_ambient ? url_ambient : restore)
                : (ambient_len > 0 ? ambient_buf : restore);
            if (in_url)
            {
                emit_color(&out, markers, cur_restore);
                if (links_bold)
                    emit_color(&out, markers, bold_off);
                if (links_italic)
                    emit_color(&out, markers, ital_off);
                if (links_underline)
                    emit_color(&out, markers, underline_off);
                in_url = 0;
            }
            dstr_append_n(&out, text + pos, seqlen);
            if ((unsigned char)text[pos] == 0x19 && seqlen < (int)sizeof(ambient_buf))
            {
                memcpy(ambient_buf, text + pos, seqlen);
                ambient_buf[seqlen] = '\0';
                ambient_len = seqlen;
            }
            else if ((unsigned char)text[pos] == 0x1C)
            {
                ambient_len = 0;
            }
            pos += seqlen;
            continue;
        }
        int remaining = len - pos;
        if (!in_url && remaining >= 8)
        {
            if (is_url(text + pos, remaining))
            {
                in_url = 1;
                url_paren_depth = 0;
                url_ambient = markers ? last_color_before(text, pos, markers) : NULL;
                emit_color(&out, markers, default_color);
                emit_color(&out, markers, link_color);
                if (links_bold)
                    emit_color(&out, markers, bold_on);
                if (links_italic)
                    emit_color(&out, markers, ital_on);
                if (links_underline)
                    emit_color(&out, markers, underline_on);
                dstr_append_char(&out, text[pos]);
                pos++;
                continue;
            }
        }
        if (in_url)
        {
            char c = text[pos];
            const char *cur_restore = markers
                ? (url_ambient ? url_ambient : restore)
                : (ambient_len > 0 ? ambient_buf : restore);
            if (c == '(')
            {
                /* Parenthese ouvrante appartenant potentiellement a l'URL
                 * elle-meme (ex: nom de fichier "...(3).pdf") : elle fait
                 * partie du lien, on avance simplement la profondeur. */
                url_paren_depth++;
                dstr_append_char(&out, c);
                pos++;
                continue;
            }
            if (c == ')' && url_paren_depth > 0)
            {
                /* Cette parenthese fermante correspond a une ouvrante
                 * rencontree dans l'URL : elle en fait partie. */
                url_paren_depth--;
                dstr_append_char(&out, c);
                pos++;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                c == ')' || c == '"' || c == ']' || c == '}' || c == '>' ||
                c == '\x0E' || c == '\x0F')
            {
                emit_color(&out, markers, cur_restore);
                if (links_bold)
                    emit_color(&out, markers, bold_off);
                if (links_italic)
                    emit_color(&out, markers, ital_off);
                if (links_underline)
                    emit_color(&out, markers, underline_off);
                in_url = 0;
                dstr_append_char(&out, c);
                pos++;
                continue;
            }
            if ((c == '.' || c == ',') && pos + 1 < len &&
                (text[pos + 1] == ' ' || text[pos + 1] == '\t' || text[pos + 1] == '\n' ||
                 (text[pos + 1] == ')' && url_paren_depth == 0) || text[pos + 1] == '"'))
            {
                emit_color(&out, markers, cur_restore);
                if (links_bold)
                    emit_color(&out, markers, bold_off);
                if (links_italic)
                    emit_color(&out, markers, ital_off);
                if (links_underline)
                    emit_color(&out, markers, underline_off);
                in_url = 0;
                dstr_append_char(&out, c);
                pos++;
                continue;
            }
            dstr_append_char(&out, c);
            pos++;
            continue;
        }
        dstr_append_char(&out, text[pos]);
        pos++;
    }
    if (in_url)
    {
        const char *cur_restore = markers
            ? (url_ambient ? url_ambient : restore)
            : (ambient_len > 0 ? ambient_buf : restore);
        emit_color(&out, markers, cur_restore);
        if (links_bold)
            emit_color(&out, markers, bold_off);
        if (links_italic)
            emit_color(&out, markers, ital_off);
        if (links_underline)
            emit_color(&out, markers, underline_off);
    }
    return out.data;
}
static char *
colorize_nicks_in_text(const char *text, char **nicks, int n_nicks,
                        const char *sender, const char *sender_color,
                        int append_sender_color, int skip_after_hash,
                        markers_t *markers)
{
    if (!nicks_enabled())
        return xstrdup(text);
    int len = strlen(text);
    dstr_t out;
    dstr_init(&out);
    int pos = 0;
    int prev_is_word = 0;
    char ambient_buf[32];
    int ambient_len = 0;
    int *nick_len = NULL;
    int bucket_head[256];
    int *bucket_next = NULL;
    for (int c = 0; c < 256; c++)
        bucket_head[c] = -1;
    if (n_nicks > 0)
    {
        nick_len = xmalloc(sizeof(int) * n_nicks);
        bucket_next = xmalloc(sizeof(int) * n_nicks);
        for (int i = n_nicks - 1; i >= 0; i--)
        {
            nick_len[i] = (int)strlen(nicks[i]);
            unsigned char c0 = nick_len[i] > 0 ? (unsigned char)nicks[i][0] : 0;
            bucket_next[i] = bucket_head[c0];
            bucket_head[c0] = i;
        }
    }
    while (pos < len)
    {
        int seqlen = match_control_seq(text, len, pos);
        if (seqlen > 0)
        {
            dstr_append_n(&out, text + pos, seqlen);
            if ((unsigned char)text[pos] == 0x19 && seqlen < (int)sizeof(ambient_buf))
            {
                memcpy(ambient_buf, text + pos, seqlen);
                ambient_buf[seqlen] = '\0';
                ambient_len = seqlen;
            }
            else if ((unsigned char)text[pos] == 0x1C)
            {
                ambient_len = 0;
            }
            pos += seqlen;
            prev_is_word = 0;
            continue;
        }
        int remaining = len - pos;
        if (!prev_is_word && remaining >= 8 && is_url(text + pos, remaining))
        {
            int spanlen = url_span_len(text, len, pos);
            dstr_append_n(&out, text + pos, spanlen);
            pos += spanlen;
            prev_is_word = 0;
            continue;
        }
        int matched = 0;
        unsigned char c0 = (unsigned char)text[pos];
        for (int i = bucket_head[c0]; i != -1; i = bucket_next[i])
        {
            const char *nick = nicks[i];
            int nl = nick_len[i];
            if (pos + nl > len)
                continue;
            if (strncmp(text + pos, nick, nl) != 0)
                continue;
            if (prev_is_word)
                continue;
            if (pos + nl < len && is_word_char(text[pos + nl]))
                continue;
            if (skip_after_hash && pos > 0 && text[pos - 1] == '#')
                continue;
            if (sender == NULL)
            {
            }
            else if (is_at_token(text, len, pos))
                continue;
            const char *color = (sender && strcmp(nick, sender) == 0)
                                     ? sender_color
                                     : get_nick_color_code(nick);
            append_formatted_nick(&out, markers, nick, color);
            if (append_sender_color)
            {
                if (markers)
                {
                    const char *amb = last_color_before(text, pos, markers);
                    emit_color(&out, markers, amb ? amb : sender_color);
                }
                else
                {
                    dstr_append(&out, ambient_len > 0 ? ambient_buf : sender_color);
                }
            }
            pos += nl;
            matched = 1;
            prev_is_word = 0;
            break;
        }
        if (!matched)
        {
            dstr_append_char(&out, text[pos]);
            prev_is_word = is_word_char(text[pos]);
            pos++;
        }
    }
    free(nick_len);
    free(bucket_next);
    return out.data;
}
static char *
colorize_nicks_input(const char *text, char **nicks, int n_nicks,
                      const char *sender, const char *sender_color,
                      const char *base_color, int skip_after_hash)
{
    int len = strlen(text);
    dstr_t out;
    dstr_init(&out);
    int pos = 0;
    int prev_is_word = 0;
    const char *base = base_color ? base_color : (sender_color ? sender_color : weechat_color("default"));
    dstr_append(&out, base);
    int nicks_on = nicks_enabled();
    int *nick_len = NULL;
    int bucket_head[256];
    int *bucket_next = NULL;
    for (int c = 0; c < 256; c++)
        bucket_head[c] = -1;
    if (nicks_on && n_nicks > 0)
    {
        nick_len = xmalloc(sizeof(int) * n_nicks);
        bucket_next = xmalloc(sizeof(int) * n_nicks);
        for (int i = n_nicks - 1; i >= 0; i--)
        {
            nick_len[i] = (int)strlen(nicks[i]);
            unsigned char c0 = nick_len[i] > 0 ? (unsigned char)nicks[i][0] : 0;
            bucket_next[i] = bucket_head[c0];
            bucket_head[c0] = i;
        }
    }
    while (pos < len)
    {
        int seqlen = match_control_seq(text, len, pos);
        if (seqlen > 0)
        {
            dstr_append_n(&out, text + pos, seqlen);
            pos += seqlen;
            prev_is_word = 0;
            dstr_append(&out, base);
            continue;
        }
        int remaining = len - pos;
        if (!prev_is_word && remaining >= 8 && is_url(text + pos, remaining))
        {
            int spanlen = url_span_len(text, len, pos);
            dstr_append_n(&out, text + pos, spanlen);
            pos += spanlen;
            prev_is_word = 0;
            continue;
        }
        int matched = 0;
        if (nicks_on)
        {
            unsigned char c0 = (unsigned char)text[pos];
            for (int i = bucket_head[c0]; i != -1; i = bucket_next[i])
            {
                const char *nick = nicks[i];
                int nl = nick_len[i];
                if (nl == 0 || pos + nl > len)
                    continue;
                if (strncmp(text + pos, nick, nl) != 0)
                    continue;
                if (prev_is_word)
                    continue;
                if (pos + nl < len && is_word_char(text[pos + nl]))
                    continue;
                if (skip_after_hash && pos > 0 && text[pos - 1] == '#')
                    continue;
                if (sender != NULL && is_at_token(text, len, pos))
                    continue;
                const char *color = (sender && strcmp(nick, sender) == 0)
                                         ? sender_color
                                         : get_nick_color_code(nick);
                dstr_append(&out, color);
                if (nicks_bold_enabled())
                    dstr_append(&out, bold_on);
                if (nicks_italic_enabled())
                    dstr_append(&out, ital_on);
                dstr_append(&out, nick);
                if (nicks_bold_enabled())
                    dstr_append(&out, bold_off);
                if (nicks_italic_enabled())
                    dstr_append(&out, ital_off);
                dstr_append(&out, base);
                pos += nl;
                matched = 1;
                prev_is_word = 0;
                break;
            }
        }
        if (!matched)
        {
            dstr_append_char(&out, text[pos]);
            prev_is_word = is_word_char(text[pos]);
            pos++;
        }
    }
    free(nick_len);
    free(bucket_next);
    return out.data;
}
static void
build_all_nicks_list(const char *buffer_ptr, const char *sender,
                      char ***out, int *n)
{
    char **others;
    int n_others;
    get_other_nicks(buffer_ptr, sender, &others, &n_others);
    int total = n_others + 1;
    char **all = xmalloc(sizeof(char *) * total);
    for (int i = 0; i < n_others; i++)
        all[i] = others[i];
    all[n_others] = xstrdup(sender);
    free(others);
    sort_nicks_by_length_desc(all, total);
    *out = all;
    *n = total;
}
#define MARK_START '\x0E'
#define MARK_END   '\x0F'
struct markers_s {
    char **items;
    int count;
    int cap;
};
static void
markers_init(markers_t *m)
{
    m->cap = 16;
    m->count = 0;
    m->items = xmalloc(sizeof(char *) * m->cap);
}
static int
markers_add(markers_t *m, const char *text, int len)
{
    if (m->count >= m->cap)
    {
        m->cap *= 2;
        m->items = xrealloc(m->items, sizeof(char *) * m->cap);
    }
    char *s = xmalloc(len + 1);
    memcpy(s, text, len);
    s[len] = '\0';
    m->items[m->count] = s;
    return m->count++;
}
static void
markers_free(markers_t *m)
{
    for (int i = 0; i < m->count; i++)
        free(m->items[i]);
    free(m->items);
}
static char *
strip_control_codes(const char *text, markers_t *markers)
{
    int len = strlen(text);
    dstr_t out;
    dstr_init(&out);
    int pos = 0;
    while (pos < len)
    {
        int seqlen = match_control_seq(text, len, pos);
        if (seqlen > 0)
        {
            int idx = markers_add(markers, text + pos, seqlen);
            char buf[32];
            snprintf(buf, sizeof(buf), "%c%d%c", MARK_START, idx, MARK_END);
            dstr_append(&out, buf);
            pos += seqlen;
            continue;
        }
        dstr_append_char(&out, text[pos]);
        pos++;
    }
    return out.data;
}
static void
neutralize_quoted_colors(char *clean, markers_t *markers)
{
    int len = strlen(clean);
    int in_quote = 0;
    int pos = 0;
    while (pos < len)
    {
        if (clean[pos] == '"')
        {
            in_quote = !in_quote;
            pos++;
            continue;
        }
        if (in_quote && clean[pos] == MARK_START)
        {
            int j = pos + 1, idx = 0, has_digit = 0;
            while (j < len && isdigit((unsigned char)clean[j]))
            {
                idx = idx * 10 + (clean[j] - '0');
                j++;
                has_digit = 1;
            }
            if (has_digit && j < len && clean[j] == MARK_END)
            {
                if (idx >= 0 && idx < markers->count)
                {
                    unsigned char c0 = (unsigned char)markers->items[idx][0];
                    if (c0 == 0x19 || c0 == 0x1C)
                    {
                        free(markers->items[idx]);
                        markers->items[idx] = xstrdup("");
                    }
                }
                pos = j + 1;
                continue;
            }
        }
        pos++;
    }
}
static char *
restore_control_codes(const char *clean, markers_t *markers)
{
    int len = strlen(clean);
    dstr_t out;
    dstr_init(&out);
    int pos = 0;
    while (pos < len)
    {
        if (clean[pos] == MARK_START)
        {
            int j = pos + 1, idx = 0, has_digit = 0;
            while (j < len && isdigit((unsigned char)clean[j]))
            {
                idx = idx * 10 + (clean[j] - '0');
                j++;
                has_digit = 1;
            }
            if (has_digit && j < len && clean[j] == MARK_END)
            {
                if (idx >= 0 && idx < markers->count)
                    dstr_append(&out, markers->items[idx]);
                pos = j + 1;
                continue;
            }
        }
        dstr_append_char(&out, clean[pos]);
        pos++;
    }
    return out.data;
}
static void
emit_marked(dstr_t *out, markers_t *markers, const char *code)
{
    int idx = markers_add(markers, code, strlen(code));
    char buf[32];
    snprintf(buf, sizeof(buf), "%c%d%c", MARK_START, idx, MARK_END);
    dstr_append(out, buf);
}
static const char *
last_color_before(const char *clean, int pos, markers_t *markers)
{
    int last_idx = -1;
    int i = 0;
    while (i < pos)
    {
        if (clean[i] == MARK_START)
        {
            int j = i + 1, idx = 0, has_digit = 0;
            while (j < pos && isdigit((unsigned char)clean[j]))
            {
                idx = idx * 10 + (clean[j] - '0');
                j++;
                has_digit = 1;
            }
            if (has_digit && j < pos && clean[j] == MARK_END)
            {
                if (idx >= 0 && idx < markers->count)
                    last_idx = idx;
                i = j + 1;
                continue;
            }
        }
        i++;
    }
    if (last_idx == -1)
        return NULL;
    for (int k = last_idx; k >= 0; k--)
    {
        const char *code = markers->items[k];
        if ((unsigned char)code[0] == 0x19)
            return code;
        if ((unsigned char)code[0] == 0x1C)
            return NULL;
    }
    return NULL;
}
static int
is_attr_toggle_code(const char *code)
{
    if (!code)
        return 0;
    return (strcmp(code, bold_on) == 0 || strcmp(code, bold_off) == 0
            || strcmp(code, ital_on) == 0 || strcmp(code, ital_off) == 0
            || strcmp(code, underline_on) == 0 || strcmp(code, underline_off) == 0);
}
/* Certains clients mIRC ont un bug de complétion de pseudo (Tab) qui laisse
 * fuiter, dans le message réellement envoyé sur le réseau, un octet de
 * bascule gras/italique/souligné isolé en PLEIN MILIEU du pseudo complété
 * (ex : "pe\x02r\x02ruche" pour "perruche", observé uniquement en début de
 * ligne chez un utilisateur mIRC). WeeChat convertit ce \x02 mIRC en son
 * code interne avant même que colorZ ne voie la ligne ; ce code isolé
 * coupe alors le mot en deux fragments et empêche colorZ de reconnaître le
 * pseudo. On neutralise ici tout marker d'attribut qui tombe strictement
 * entre deux caractères de mot, afin que le pseudo redevienne contigu pour
 * le matching (le fragment de mise en forme parasite est donc supprimé du
 * résultat final, plutôt que laissé tel quel). */
static void
neutralize_midword_attr_markers(char **clean_ptr, markers_t *markers)
{
    char *clean = *clean_ptr;
    int len = (int)strlen(clean);
    dstr_t out;
    dstr_init(&out);
    int pos = 0;
    while (pos < len)
    {
        if (clean[pos] == MARK_START)
        {
            int j = pos + 1, idx = 0, has_digit = 0;
            while (j < len && isdigit((unsigned char)clean[j]))
            {
                idx = idx * 10 + (clean[j] - '0');
                j++;
                has_digit = 1;
            }
            if (has_digit && j < len && clean[j] == MARK_END)
            {
                int prev_word = (out.len > 0)
                                 && is_word_char(out.data[out.len - 1]);
                int next_word = (j + 1 < len) && is_word_char(clean[j + 1]);
                if (prev_word && next_word && idx >= 0 && idx < markers->count
                    && is_attr_toggle_code(markers->items[idx]))
                {
                    pos = j + 1;
                    continue;
                }
            }
        }
        dstr_append_char(&out, clean[pos]);
        pos++;
    }
    free(clean);
    *clean_ptr = out.data;
}
static char *
strip_all_control_codes(const char *text)
{
    int len = strlen(text);
    dstr_t out;
    dstr_init(&out);
    int pos = 0;
    while (pos < len)
    {
        int seqlen = match_control_seq(text, len, pos);
        if (seqlen > 0)
        {
            pos += seqlen;
            continue;
        }
        dstr_append_char(&out, text[pos]);
        pos++;
    }
    return out.data;
}
static char *
apply_sender_color_to_text(const char *text, const char *sender_color)
{
    if (!text || !text[0])
        return xstrdup(text ? text : "");
    dstr_t out;
    dstr_init(&out);
    if ((unsigned char)text[0] != 0x19)
        dstr_append(&out, sender_color);
    for (const char *p = text; *p; p++)
    {
        dstr_append_char(&out, *p);
        if ((unsigned char)*p == 0x1C)
            dstr_append(&out, sender_color);
    }
    return out.data;
}
static char *
format_mentions_port(const char *line, const char *buffer_ptr,
                      const char *sender, const char *sender_color,
                      int add_sender_color,
                      const char *self_nick, const char *self_color)
{
    if (!nicks_enabled())
        return xstrdup(line);
    char **others;
    int n_others;
    get_other_nicks(buffer_ptr, sender, &others, &n_others);
    if (self_nick && self_nick[0] && self_color)
    {
        int already_present = 0;
        for (int i = 0; i < n_others; i++)
            if (strcmp(others[i], self_nick) == 0)
                already_present = 1;
        if (!already_present)
        {
            others = xrealloc(others, sizeof(char *) * (n_others + 1));
            others[n_others] = xstrdup(self_nick);
            n_others++;
        }
    }
    if (n_others == 0)
    {
        free_nick_list(others, n_others);
        return xstrdup(line);
    }
    sort_nicks_by_length_desc(others, n_others);
    int *nick_len = xmalloc(sizeof(int) * n_others);
    int bucket_head[256];
    int *bucket_next = xmalloc(sizeof(int) * n_others);
    for (int c = 0; c < 256; c++)
        bucket_head[c] = -1;
    for (int i = n_others - 1; i >= 0; i--)
    {
        nick_len[i] = (int)strlen(others[i]);
        unsigned char c0 = nick_len[i] > 0 ? (unsigned char)others[i][0] : 0;
        bucket_next[i] = bucket_head[c0];
        bucket_head[c0] = i;
    }
    markers_t markers;
    markers_init(&markers);
    char *clean = strip_control_codes(line, &markers);
    neutralize_midword_attr_markers(&clean, &markers);
    int clen = strlen(clean);
    dstr_t out;
    dstr_init(&out);
    int pos = 0;
    int prev_is_word = 0;
    while (pos < clen)
    {
        int remaining = clen - pos;
        if (!prev_is_word && remaining >= 8 && is_url(clean + pos, remaining))
        {
            int spanlen = url_span_len(clean, clen, pos);
            dstr_append_n(&out, clean + pos, spanlen);
            pos += spanlen;
            prev_is_word = 0;
            continue;
        }
        int matched = 0;
        unsigned char c0 = (unsigned char)clean[pos];
        for (int i = bucket_head[c0]; i != -1; i = bucket_next[i])
        {
            const char *nick = others[i];
            int nl = nick_len[i];
            if (pos + nl > clen)
                continue;
            if (prev_is_word)
                continue;
            if (strncmp(clean + pos, nick, nl) != 0)
                continue;
            if (pos + nl < clen && is_word_char(clean[pos + nl]))
                continue;
            if (is_at_token(clean, clen, pos))
                continue;
            const char *color = (self_nick && self_color && strcmp(nick, self_nick) == 0)
                                     ? self_color
                                     : get_nick_color_code(nick);
            dstr_append(&out, color);
            if (nicks_bold_enabled())
                dstr_append(&out, bold_on);
            if (nicks_italic_enabled())
                dstr_append(&out, ital_on);
            dstr_append(&out, nick);
            if (nicks_bold_enabled())
                dstr_append(&out, bold_off);
            if (nicks_italic_enabled())
                dstr_append(&out, ital_off);
            if (add_sender_color)
            {
                const char *amb = last_color_before(clean, pos, &markers);
                dstr_append(&out, amb ? amb : sender_color);
            }
            pos += nl;
            matched = 1;
            prev_is_word = 0;
            break;
        }
        if (!matched)
        {
            dstr_append_char(&out, clean[pos]);
            prev_is_word = is_word_char(clean[pos]);
            pos++;
        }
    }
    free(clean);
    free(nick_len);
    free(bucket_next);
    char *result = restore_control_codes(out.data, &markers);
    free(out.data);
    markers_free(&markers);
    free_nick_list(others, n_others);
    return result;
}
static int
channel_bold_enabled(void)
{
    return g_opts.channel_bold;
}
static int
channel_italic_enabled(void)
{
    return g_opts.channel_italic;
}
static char *
colorize_channel(const char *line, const char *buffer_ptr, const char *sender_color)
{
    if (!channel_enabled())
        return xstrdup(line);
    struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
    if (!buffer)
        return xstrdup(line);
    const char *channel_name = weechat_buffer_get_string(buffer, "localvar_channel");
    if (!channel_name || !channel_name[0])
        return xstrdup(line);
    const char *server_name = weechat_buffer_get_string(buffer, "localvar_server");
    if (server_name && server_name[0] && strcmp(channel_name, server_name) == 0)
    {
        return xstrdup(line);
    }
    markers_t markers;
    markers_init(&markers);
    char *clean = strip_control_codes(line, &markers);
    int cl = strlen(channel_name);
    int clen = strlen(clean);
    dstr_t out;
    dstr_init(&out);
    const char *channel_color_code = weechat_color(weechat_config_get_plugin("channel_color"));
    int use_bold = channel_bold_enabled();
    int use_italic = channel_italic_enabled();
    int pos = 0;
    while (pos < clen)
    {
        if (pos + cl <= clen && strncmp(clean + pos, channel_name, cl) == 0
            && (pos == 0 || !is_word_char(clean[pos - 1]))
            && (pos + cl >= clen || !is_word_char(clean[pos + cl]))
            && !is_at_token(clean, clen, pos))
        {
            dstr_append(&out, channel_color_code);
            if (use_bold)
                dstr_append(&out, bold_on);
            if (use_italic)
                dstr_append(&out, ital_on);
            dstr_append(&out, channel_name);
            if (use_bold)
                dstr_append(&out, bold_off);
            if (use_italic)
                dstr_append(&out, ital_off);
            if (sender_color)
            {
                const char *amb = last_color_before(clean, pos, &markers);
                dstr_append(&out, amb ? amb : sender_color);
            }
            pos += cl;
            continue;
        }
        dstr_append_char(&out, clean[pos]);
        pos++;
    }
    free(clean);
    char *result = restore_control_codes(out.data, &markers);
    free(out.data);
    markers_free(&markers);
    return result;
}
static char *
get_self_color(const char *buffer_ptr)
{
    struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
    if (!buffer)
        return NULL;
    const char *server = weechat_buffer_get_string(buffer, "localvar_server");
    if (!server || !server[0])
        return NULL;
    const char *self_nick = weechat_info_get("irc_nick", server);
    if (!self_nick || !self_nick[0])
        return NULL;
    struct t_config_option *opt = weechat_config_get("weechat.color.chat_nick_self");
    if (!opt)
        return NULL;
    const char *name = weechat_config_string(opt);
    if (!name || !name[0])
        return NULL;
    return xstrdup(weechat_color(name));
}
static int
is_sep_char(unsigned char c)
{
    if (c == 0x1C)
        return 1;
    if (isspace(c) || ispunct(c))
        return 1;
    return 0;
}
static int
find_sender_colorcode(const char *line, int len, const char *sender,
                       int sender_len, int *seqlen_out)
{
    for (int pos = 0; pos < len; pos++)
    {
        if ((unsigned char)line[pos] != 0x19)
            continue;
        int seqlen = match_control_seq(line, len, pos);
        if (seqlen <= 0)
            continue;
        if (pos + seqlen + sender_len <= len
            && strncmp(line + pos + seqlen, sender, sender_len) == 0)
        {
            *seqlen_out = seqlen;
            return pos;
        }
    }
    return -1;
}
static char *
process_message(const char *line, const char *buffer_ptr,
                 const char *sender, const char *sender_color,
                 const char *link_color,
                 const char *self_nick, const char *self_color)
{
    char *result;
    int len = strlen(line);
    int sender_len = strlen(sender);
    int seqlen = 0;
    int match_pos = find_sender_colorcode(line, len, sender, sender_len, &seqlen);
    if (match_pos >= 0)
    {
        int nick_start = match_pos + seqlen;
        int nick_end = nick_start + sender_len;
        int sep_is_valid = (nick_end < len) && is_sep_char((unsigned char)line[nick_end]);
        int after_start = sep_is_valid ? nick_end + 1 : nick_end;
        dstr_t out;
        dstr_init(&out);
        dstr_append_n(&out, line, match_pos);
        if (nicks_enabled())
            append_formatted_nick(&out, NULL, sender, sender_color);
        else
            dstr_append_n(&out, line + match_pos, seqlen + sender_len);
        if (sep_is_valid)
        {
            if (lines_enabled())
                dstr_append(&out, sender_color);
            dstr_append_char(&out, line[nick_end]);
        }
        if (lines_enabled())
        {
            char *after_colored = apply_sender_color_to_text(line + after_start, sender_color);
            dstr_append(&out, after_colored);
            free(after_colored);
        }
        else
        {
            dstr_append(&out, line + after_start);
        }
        result = out.data;
    }
    else
    {
        result = xstrdup(line);
    }
    char *with_mentions = format_mentions_port(result, buffer_ptr, sender, sender_color, 1,
                                                 self_nick, self_color);
    free(result);
    char *with_links = colorize_links_in_text(with_mentions, link_color, sender_color, NULL);
    free(with_mentions);
    return with_links;
}
static char *
process_mode(const char *line, const char *buffer_ptr,
             const char *sender, const char *sender_color,
             const char *link_color)
{
    int len = strlen(line);
    int p = 0;
    dstr_t prefix;
    dstr_init(&prefix);
    if (p + 2 < len && (unsigned char)line[p] == 0x19
        && isdigit((unsigned char)line[p + 1]) && isdigit((unsigned char)line[p + 2]))
    {
        int consumed = 3;
        if (p + 5 < len && (unsigned char)line[p + 3] == 0x19
            && isdigit((unsigned char)line[p + 4]) && isdigit((unsigned char)line[p + 5]))
            consumed = 6;
        dstr_append_n(&prefix, line + p, consumed);
        p += consumed;
    }
    int tab_pos = -1;
    for (int i = p; i < len; i++)
        if (line[i] == '\x09') { tab_pos = i; break; }
    if (tab_pos >= 0)
    {
        dstr_append_n(&prefix, line + p, tab_pos - p + 1);
        p = tab_pos + 1;
    }
    else if (p + 2 < len && (unsigned char)line[p] == 0x19
             && isdigit((unsigned char)line[p + 1]) && isdigit((unsigned char)line[p + 2]))
    {
        dstr_append_n(&prefix, line + p, 3);
        p += 3;
    }
    char *rest_stripped = strip_all_control_codes(line + p);
    char **all_nicks;
    int n_all;
    build_all_nicks_list(buffer_ptr, sender, &all_nicks, &n_all);
    debugf(3, "process_mode: sender='%s', %d nick(s) connus:", sender, n_all);
    for (int i = 0; i < n_all; i++)
        debugf(3, "  - '%s' (len=%d)", all_nicks[i], (int)strlen(all_nicks[i]));
    debug_hex(4, "process_mode rest_stripped", rest_stripped);
    char *rest_colored = colorize_nicks_in_text(rest_stripped, all_nicks, n_all, sender,
                                                 sender_color, 1, 0, NULL);
    free_nick_list(all_nicks, n_all);
    free(rest_stripped);
    char *with_links = colorize_links_in_text(rest_colored, link_color, sender_color, NULL);
    free(rest_colored);
    dstr_t out;
    dstr_init(&out);
    dstr_append(&out, prefix.data);
    if (lines_enabled() && (unsigned char)with_links[0] != 0x19)
        dstr_append(&out, sender_color);
    dstr_append(&out, with_links);
    free(with_links);
    free(prefix.data);
    return out.data;
}
static char *
process_system_event(const char *line, const char *sender, const char *sender_color,
                      const char *event_type, const char *buffer_ptr,
                      const char *link_color, const char *self_nick)
{
    (void)self_nick;
    int len = strlen(line);
    int prefix_len = 0;
    if (len >= 3 && (unsigned char)line[0] == 0x19
        && isdigit((unsigned char)line[1]) && isdigit((unsigned char)line[2]))
    {
        int i = 3;
        while (i < len && (unsigned char)line[i] != 0x19 && line[i] != '\x09')
            i++;
        if (i < len && line[i] == '\x09')
            prefix_len = i + 1;
    }
    int is_chghost_event = (strcmp(event_type, "chghost") == 0);
    int is_join_part_quit = (strcmp(event_type, "join") == 0 ||
                             strcmp(event_type, "part") == 0 ||
                             strcmp(event_type, "quit") == 0);
    int should_colorize_join_part_quit = hostmask_nicks_enabled() && is_join_part_quit;
    int should_colorize_other = events_enabled() && !is_join_part_quit;
    int should_highlight_nicks_other = events_enabled() && !is_join_part_quit;
    int add_sender_color =
        (strcmp(event_type, "nick") == 0 || strcmp(event_type, "account") == 0);
    const char *default_color = weechat_color("default");
    const char *base_color = is_chghost_event ? default_color : sender_color;
    dstr_t rest1;
    dstr_init(&rest1);
    if (should_colorize_join_part_quit || should_colorize_other || is_chghost_event)
    {
        dstr_append(&rest1, base_color);
        for (const char *p = line + prefix_len; *p; p++)
        {
            dstr_append_char(&rest1, *p);
            if ((unsigned char)*p == 0x1C)
                dstr_append(&rest1, base_color);
        }
    }
    else
    {
        dstr_append(&rest1, line + prefix_len);
    }
    char **others;
    int n_others;
    get_other_nicks(buffer_ptr, sender, &others, &n_others);
    int total = n_others + 1;
    char **all = xmalloc(sizeof(char *) * total);
    for (int i = 0; i < n_others; i++)
        all[i] = others[i];
    all[n_others] = xstrdup(sender);
    free(others);
    sort_nicks_by_length_desc(all, total);
    markers_t markers;
    markers_init(&markers);
    char *clean = strip_control_codes(rest1.data, &markers);
    neutralize_midword_attr_markers(&clean, &markers);
    free(rest1.data);
    if (should_colorize_join_part_quit || should_highlight_nicks_other)
    {
        int use_bold = is_join_part_quit
            ? hostmask_nicks_bold_enabled() : events_bold_enabled();
        int use_italic = is_join_part_quit
            ? hostmask_nicks_italic_enabled() : events_italic_enabled();
        int clen = strlen(clean);
        int *nick_len = xmalloc(sizeof(int) * total);
        int bucket_head[256];
        int *bucket_next = xmalloc(sizeof(int) * total);
        for (int c = 0; c < 256; c++)
            bucket_head[c] = -1;
        for (int i = total - 1; i >= 0; i--)
        {
            nick_len[i] = (int)strlen(all[i]);
            unsigned char c0 = nick_len[i] > 0 ? (unsigned char)all[i][0] : 0;
            bucket_next[i] = bucket_head[c0];
            bucket_head[c0] = i;
        }
        dstr_t newclean;
        dstr_init(&newclean);
        int pos = 0;
        while (pos < clen)
        {
            int prev_word = (pos > 0) && is_word_char(clean[pos - 1]);
            int remaining_url = clen - pos;
            if (!prev_word && remaining_url >= 8 && is_url(clean + pos, remaining_url))
            {
                int spanlen = url_span_len(clean, clen, pos);
                dstr_append_n(&newclean, clean + pos, spanlen);
                pos += spanlen;
                continue;
            }
            int is_hash = (pos > 0 && clean[pos - 1] == '#');
            int matched = 0;
            if (!is_hash && !prev_word)
            {
                unsigned char c0 = (unsigned char)clean[pos];
                for (int i = bucket_head[c0]; i != -1; i = bucket_next[i])
                {
                    const char *nick = all[i];
                    int nl = nick_len[i];
                    if (pos + nl > clen)
                        continue;
                    if (strncmp(clean + pos, nick, nl) != 0)
                        continue;
                    if (pos + nl < clen && is_word_char(clean[pos + nl]))
                        continue;
                    const char *color = (strcmp(nick, sender) == 0)
                                             ? sender_color : get_nick_color_code(nick);
                    const char *ambient = last_color_before(newclean.data, (int)newclean.len, &markers);
                    const char *amb = ambient ? ambient : base_color;
                    emit_marked(&newclean, &markers, color);
                    if (use_bold)
                        emit_marked(&newclean, &markers, bold_on);
                    if (use_italic)
                        emit_marked(&newclean, &markers, ital_on);
                    dstr_append_n(&newclean, nick, nl);
                    if (use_bold)
                        emit_marked(&newclean, &markers, bold_off);
                    if (use_italic)
                        emit_marked(&newclean, &markers, ital_off);
                    emit_marked(&newclean, &markers, amb);
                    if (add_sender_color && strcmp(amb, sender_color) != 0)
                        emit_marked(&newclean, &markers, sender_color);
                    pos += nl;
                    matched = 1;
                    break;
                }
            }
            if (!matched)
            {
                dstr_append_char(&newclean, clean[pos]);
                pos++;
            }
        }
        free(nick_len);
        free(bucket_next);
        free(clean);
        clean = newclean.data;
    }
    char *rest_final = restore_control_codes(clean, &markers);
    free(clean);
    markers_free(&markers);
    for (int i = 0; i < total; i++)
        free(all[i]);
    free(all);
    char *with_links = colorize_links_in_text(rest_final, link_color, base_color, NULL);
    free(rest_final);
    dstr_t out;
    dstr_init(&out);
    dstr_append_n(&out, line, prefix_len);
    dstr_append(&out, default_color);
    dstr_append(&out, with_links);
    free(with_links);
    return out.data;
}
static char *
process_333_event(const char *line, const char *buffer_ptr, const char *link_color,
                   const char *author_nick)
{
    debugf(2, "Événement irc_333 (topic info) - colorisation des nicks");
    struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
    if (!buffer)
        return xstrdup(line);
    const char *channel = weechat_buffer_get_string(buffer, "localvar_channel");
    const char *real_setter = NULL;
    if (channel && channel[0] && last_333_channel[0]
        && strcmp(channel, last_333_channel) == 0)
    {
        real_setter = last_333_setter;
        debugf(3, "irc_333 : auteur réel capturé '%s' au lieu de '%s'",
               real_setter, author_nick ? author_nick : "");
    }
    char **all_nicks;
    int n_all;
    get_nicks_with_author(buffer_ptr, "",
                           real_setter ? real_setter : author_nick,
                           &all_nicks, &n_all);
    if (n_all > 0)
    {
        sort_nicks_by_length_desc(all_nicks, n_all);
        debugf(3, "Liste des nicks pour colorisation (%d):", n_all);
        for (int i = 0; i < n_all; i++)
            debugf(3, "  - '%s'", all_nicks[i]);
    }
    else
    {
        debugf(3, "Aucun nick trouvé pour colorisation");
    }
    markers_t markers;
    markers_init(&markers);
    char *clean = strip_control_codes(line, &markers);
    neutralize_midword_attr_markers(&clean, &markers);
    debugf(4, "Texte nettoyé : %s", clean);
    char *colored_clean = NULL;
    if (n_all > 0 && clean && clean[0])
    {
        colored_clean = colorize_nicks_in_text(clean, all_nicks, n_all,
                                                NULL, "\x1C", 1, 0, &markers);
        debugf(3, "colorize_nicks_in_text applique sur %d nicks", n_all);
    }
    else if (clean)
    {
        colored_clean = xstrdup(clean);
        debugf(3, "Aucun nick, texte non modifié");
    }
    else
    {
        colored_clean = xstrdup(line);
    }
    char *with_links = colorize_links_in_text(colored_clean, link_color, "\x1C", &markers);
    free(colored_clean);
    char *restored = restore_control_codes(with_links, &markers);
    free(with_links);
    free(clean);
    markers_free(&markers);
    char *result = colorize_channel(restored, buffer_ptr, "\x1C");
    free(restored);
    debug_hex(4, "irc_333 final_out", result);
    free_nick_list(all_nicks, n_all);
    return result;
}
static void
set_default(const char *option, const char *value, const char *desc)
{
    if (!weechat_config_get_plugin(option) || !weechat_config_get_plugin(option)[0])
        weechat_config_set_plugin(option, value);
    weechat_config_set_desc_plugin(option, desc);
}
static void
migrate_renamed_option(const char *old_name, const char *new_name)
{
    if (weechat_config_is_set_plugin(old_name))
    {
        if (!weechat_config_is_set_plugin(new_name))
            weechat_config_set_plugin(new_name, weechat_config_get_plugin(old_name));
        weechat_config_unset_plugin(old_name);
    }
}
static int
cmd_colorZdebug_cb(const void *pointer, void *data, struct t_gui_buffer *buffer,
                   int argc, char **argv, char **argv_eol)
{
    (void)pointer; (void)data; (void)buffer; (void)argv;
    const char *args = (argc > 1) ? argv_eol[1] : "";
    while (*args == ' ')
        args++;
    int valid = args[0] != '\0';
    for (const char *p = args; *p; p++)
        if (!isdigit((unsigned char)*p))
            valid = 0;
    int val = valid ? atoi(args) : -1;
    if (!valid || val < 0 || val > 7)
    {
        weechat_printf(NULL, "colorZ: /colorZdebug <0-7> (actuel: %s)",
                        weechat_config_get_plugin("debug"));
        return WEECHAT_RC_OK;
    }
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", val);
    weechat_config_set_plugin("debug", buf);
    weechat_printf(NULL, "colorZ: niveau debug = %d", val);
    debugf(1, "Niveau debug changé à %d", val);
    return WEECHAT_RC_OK;
}
static char *
colorize_self_nick_only(const char *text, const char *self_nick,
                         const char *self_color, const char *default_color)
{
    if (!nicks_enabled() || !self_nick || !self_nick[0])
        return xstrdup(text);
    int len = strlen(text);
    int nl = strlen(self_nick);
    dstr_t out;
    dstr_init(&out);
    int pos = 0;
    int prev_is_word = 0;
    while (pos < len)
    {
        int seqlen = match_control_seq(text, len, pos);
        if (seqlen > 0)
        {
            dstr_append_n(&out, text + pos, seqlen);
            pos += seqlen;
            prev_is_word = 0;
            continue;
        }
        if (!prev_is_word && pos + nl <= len
            && strncmp(text + pos, self_nick, nl) == 0
            && (pos + nl >= len || !is_word_char(text[pos + nl])))
        {
            append_formatted_nick(&out, NULL, self_nick, self_color);
            dstr_append(&out, default_color);
            pos += nl;
            prev_is_word = 0;
            continue;
        }
        dstr_append_char(&out, text[pos]);
        prev_is_word = is_word_char(text[pos]);
        pos++;
    }
    return out.data;
}
static char *
extract_line_prefix(const char *line, const char **body_out)
{
    int len = strlen(line);
    int p = 0;
    dstr_t pfx;
    dstr_init(&pfx);
    if (p + 2 < len && (unsigned char)line[p] == 0x19
        && isdigit((unsigned char)line[p + 1]) && isdigit((unsigned char)line[p + 2]))
    {
        int consumed = 3;
        if (p + 5 < len && (unsigned char)line[p + 3] == 0x19
            && isdigit((unsigned char)line[p + 4]) && isdigit((unsigned char)line[p + 5]))
            consumed = 6;
        dstr_append_n(&pfx, line + p, consumed);
        p += consumed;
    }
    int tab_pos = -1;
    for (int i = p; i < len; i++)
        if (line[i] == '\x09') { tab_pos = i; break; }
    if (tab_pos >= 0)
    {
        dstr_append_n(&pfx, line + p, tab_pos - p + 1);
        p = tab_pos + 1;
    }
    *body_out = line + p;
    return pfx.data;
}
static char *
scan_word_stop_space(const char *body, int len, int *pos_io)
{
    int pos = *pos_io;
    dstr_t out;
    dstr_init(&out);
    while (pos < len)
    {
        int sl = match_control_seq(body, len, pos);
        if (sl > 0)
        {
            pos += sl;
            continue;
        }
        if (body[pos] == ' ')
            break;
        dstr_append_char(&out, body[pos]);
        pos++;
    }
    *pos_io = pos;
    if (out.len == 0)
    {
        free(out.data);
        return NULL;
    }
    return out.data;
}
static void
append_formatted_channel(dstr_t *out, const char *channel_name)
{
    const char *channel_color_code = weechat_color(weechat_config_get_plugin("channel_color"));
    int use_bold = channel_bold_enabled();
    int use_italic = channel_italic_enabled();
    dstr_append(out, channel_color_code);
    if (use_bold)
        dstr_append(out, bold_on);
    if (use_italic)
        dstr_append(out, ital_on);
    dstr_append(out, channel_name);
    if (use_bold)
        dstr_append(out, bold_off);
    if (use_italic)
        dstr_append(out, ital_off);
}
/* Détecte un crochet "[#canal]" en tête de ligne (format utilisé par WeeChat
 * quand une réponse WHO/whois/End-of-WHO est imprimée sur le buffer serveur,
 * pas sur le buffer du canal lui-même : le nom du canal vient donc du texte
 * du message, et non de localvar_channel). Retourne le nom du canal alloué
 * (sans les crochets) si trouvé, NULL sinon ; *after_pos pointe juste après
 * le crochet fermant dans tous les cas où on est entré dans un "[...]". */
static char *
detect_leading_channel(const char *body, int len, int start_pos,
                        int *channel_start, int *channel_end, int *after_pos)
{
    *channel_start = -1;
    *channel_end = -1;
    *after_pos = start_pos;
    if (start_pos >= len || body[start_pos] != '[')
        return NULL;
    int open_pos = start_pos;
    int scan_pos = start_pos + 1;
    char *content = scan_word_stop_space(body, len, &scan_pos);
    if (!content)
        return NULL;
    size_t clen = strlen(content);
    if (clen > 0 && content[clen - 1] == ']')
    {
        content[clen - 1] = '\0';
        clen--;
    }
    if (clen > 0 && content[0] == '#')
    {
        *channel_start = open_pos;
        *channel_end = scan_pos;
        *after_pos = scan_pos;
        return content;
    }
    free(content);
    return NULL;
}
static char *
extract_target_nick(const char *body, int *bracket_start, int *bracket_end,
                     char **channel_out, int *channel_start, int *channel_end)
{
    int len = (int)strlen(body);
    int pos = 0;
    *bracket_start = -1;
    *bracket_end = -1;
    *channel_out = NULL;
    while (pos < len)
    {
        int seqlen = match_control_seq(body, len, pos);
        if (seqlen == 0)
            break;
        pos += seqlen;
    }
    int after_pos = pos;
    char *chan = detect_leading_channel(body, len, pos, channel_start, channel_end, &after_pos);
    if (chan)
    {
        *channel_out = chan;
        pos = after_pos;
        if (pos < len && body[pos] == ' ')
            pos++;
        return scan_word_stop_space(body, len, &pos);
    }
    if (pos < len && body[pos] == '[')
    {
        int open_pos = pos;
        int scan_pos = pos + 1;
        char *content = scan_word_stop_space(body, len, &scan_pos);
        if (content)
        {
            size_t clen = strlen(content);
            if (clen > 0 && content[clen - 1] == ']')
            {
                content[clen - 1] = '\0';
                clen--;
            }
            if (clen > 0)
            {
                *bracket_start = open_pos;
                *bracket_end = scan_pos;
                return content;
            }
        }
        free(content);
        return NULL;
    }
    return scan_word_stop_space(body, len, &pos);
}
static char *
modifier_cb_impl(const void *pointer, void *data, const char *modifier,
                  const char *modifier_data, const char *string)
{
    (void)pointer; (void)data; (void)modifier;
    char *mdata_copy = xstrdup(modifier_data);
    char *semicolon = strchr(mdata_copy, ';');
    char *buffer_ptr = mdata_copy;
    char *tags_str = "";
    if (semicolon)
    {
        *semicolon = '\0';
        tags_str = semicolon + 1;
    }
    debug_hex(5, "modifier_data_raw", modifier_data);
    debug_hex(5, "buffer_ptr_extrait", buffer_ptr);
    int is_privmsg = 0, is_action = 0, is_notice = 0, is_mode = 0;
    int is_join = 0, is_part = 0, is_quit = 0, is_kick = 0, is_nick_ev = 0;
    int is_chghost = 0, is_topic = 0, is_account = 0;
    int is_324 = 0, is_329 = 0, is_331 = 0, is_332 = 0, is_333 = 0, is_353 = 0, is_366 = 0;
    int is_221 = 0, is_396 = 0, is_372 = 0, is_001 = 0, is_900 = 0;
    int is_352 = 0, is_whois = 0, is_315 = 0;
    int is_topic_event = 0;
    char sender[512] = "";
    char *tags_copy = xstrdup(tags_str);
    char *saveptr = NULL;
    char *tok = strtok_r(tags_copy, ",", &saveptr);
    while (tok)
    {
        if (strncmp(tok, "nick_", 5) == 0)
            snprintf(sender, sizeof(sender), "%s", tok + 5);
        else if (strcmp(tok, "irc_privmsg") == 0) is_privmsg = 1;
        else if (strcmp(tok, "irc_action") == 0) is_action = 1;
        else if (strcmp(tok, "irc_notice") == 0) is_notice = 1;
        else if (strcmp(tok, "irc_mode") == 0) is_mode = 1;
        else if (strcmp(tok, "irc_join") == 0) is_join = 1;
        else if (strcmp(tok, "irc_part") == 0) is_part = 1;
        else if (strcmp(tok, "irc_quit") == 0) is_quit = 1;
        else if (strcmp(tok, "irc_kick") == 0) is_kick = 1;
        else if (strcmp(tok, "irc_nick") == 0) is_nick_ev = 1;
        else if (strcmp(tok, "irc_chghost") == 0) is_chghost = 1;
        else if (strcmp(tok, "irc_topic") == 0) { is_topic = 1; is_topic_event = 1; }
        else if (strcmp(tok, "irc_account") == 0) is_account = 1;
        else if (strcmp(tok, "irc_324") == 0) is_324 = 1;
        else if (strcmp(tok, "irc_329") == 0) is_329 = 1;
        else if (strcmp(tok, "irc_331") == 0) is_331 = 1;
        else if (strcmp(tok, "irc_332") == 0) is_332 = 1;
        else if (strcmp(tok, "irc_333") == 0) is_333 = 1;
        else if (strcmp(tok, "irc_353") == 0) is_353 = 1;
        else if (strcmp(tok, "irc_366") == 0) is_366 = 1;
        else if (strcmp(tok, "irc_221") == 0) is_221 = 1;
        else if (strcmp(tok, "irc_396") == 0) is_396 = 1;
        else if (strcmp(tok, "irc_372") == 0) is_372 = 1;
        else if (strcmp(tok, "irc_001") == 0) is_001 = 1;
        else if (strcmp(tok, "irc_900") == 0) is_900 = 1;
        else if (strcmp(tok, "irc_352") == 0) is_352 = 1;
        else if (strcmp(tok, "irc_315") == 0) is_315 = 1;
        else if (strcmp(tok, "irc_311") == 0) is_whois = 1;
        else if (strcmp(tok, "irc_312") == 0) is_whois = 1;
        else if (strcmp(tok, "irc_313") == 0) is_whois = 1;
        else if (strcmp(tok, "irc_317") == 0) is_whois = 1;
        else if (strcmp(tok, "irc_318") == 0) is_whois = 1;
        else if (strcmp(tok, "irc_319") == 0) is_whois = 1;
        else if (strcmp(tok, "irc_320") == 0) is_whois = 1;
        else if (strcmp(tok, "irc_330") == 0) is_whois = 1;
        else if (strcmp(tok, "irc_338") == 0) is_whois = 1;
        else if (strcmp(tok, "irc_378") == 0) is_whois = 1;
        else if (strcmp(tok, "irc_379") == 0) is_whois = 1;
        else if (strcmp(tok, "irc_671") == 0) is_whois = 1;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    free(tags_copy);
    const char *link_color_config = weechat_config_get_plugin("links_color");
    if (!link_color_config || !link_color_config[0])
        link_color_config = "white";
    const char *link_color = link_color_config;
    int is_events_block = is_221 || is_396 || is_324 || is_329
        || is_353 || is_366 || is_333 || is_331 || is_332 || is_topic_event
        || is_900 || is_352 || is_whois || is_315;
    if (is_events_block && !events_enabled())
    {
        char *result = xstrdup(string);
        free(mdata_copy);
        return result;
    }
    if (is_001)
    {
        debugf(2, "irc_001 (welcome) - colorisation du nick uniquement");
        struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
        if (!buffer)
        {
            free(mdata_copy);
            return NULL;
        }
        const char *default_color = weechat_color("default");
        const char *srv = weechat_buffer_get_string(buffer, "localvar_server");
        const char *self_nick = srv ? weechat_info_get("irc_nick", srv) : NULL;
        char *self_color_owned = NULL;
        if (self_nick && self_nick[0])
        {
            self_color_owned = get_self_color(buffer_ptr);
            if (!self_color_owned)
                self_color_owned = xstrdup(get_nick_color_code(self_nick));
        }
        const char *body = NULL;
        char *prefix_str = extract_line_prefix(string, &body);
        char *colored_body = colorize_self_nick_only(body, self_nick,
                                                       self_color_owned ? self_color_owned : default_color,
                                                       "\x1C");
        dstr_t out;
        dstr_init(&out);
        dstr_append(&out, prefix_str);
        dstr_append(&out, colored_body);
        free(prefix_str);
        free(colored_body);
        free(self_color_owned);
        char *result = out.data;
        debug_hex(4, "irc_001 final_out", result);
        free(mdata_copy);
        return result;
    }
    if (is_372)
    {
        debugf(2, "irc_372 (MOTD) - colorisation des liens uniquement");
        const char *default_color = weechat_color("default");
        const char *body = NULL;
        char *prefix_str = extract_line_prefix(string, &body);
        char *colored_body = colorize_links_in_text(body, link_color, default_color, NULL);
        dstr_t out;
        dstr_init(&out);
        dstr_append(&out, prefix_str);
        dstr_append(&out, colored_body);
        free(prefix_str);
        free(colored_body);
        char *result = out.data;
        debug_hex(4, "irc_372 final_out", result);
        free(mdata_copy);
        return result;
    }
    if (is_221 || is_396)
    {
        debugf(2, "Événement mode utilisateur (irc_221/396) - colorisation du nick uniquement");
        struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
        if (!buffer)
        {
            free(mdata_copy);
            return NULL;
        }
        const char *default_color = weechat_color("default");
        const char *srv = weechat_buffer_get_string(buffer, "localvar_server");
        const char *self_nick = srv ? weechat_info_get("irc_nick", srv) : NULL;
        char *self_color_owned = NULL;
        if (self_nick && self_nick[0])
        {
            self_color_owned = get_self_color(buffer_ptr);
            if (!self_color_owned)
                self_color_owned = xstrdup(get_nick_color_code(self_nick));
        }
        const char *body = NULL;
        char *prefix_str = extract_line_prefix(string, &body);
        char *colored_body = colorize_self_nick_only(body, self_nick,
                                                       self_color_owned ? self_color_owned : default_color,
                                                       "\x1C");
        dstr_t out;
        dstr_init(&out);
        dstr_append(&out, prefix_str);
        dstr_append(&out, colored_body);
        free(prefix_str);
        free(colored_body);
        free(self_color_owned);
        char *result = out.data;
        debug_hex(4, "irc_221/396 final_out", result);
        free(mdata_copy);
        return result;
    }
    if (is_900)
    {
        debugf(2, "Événement irc_900 (identification) - colorisation de toutes "
                   "les occurrences du pseudo local dans le message");
        struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
        if (!buffer)
        {
            free(mdata_copy);
            return NULL;
        }
        const char *default_color = weechat_color("default");
        const char *srv = weechat_buffer_get_string(buffer, "localvar_server");
        const char *self_nick = srv ? weechat_info_get("irc_nick", srv) : NULL;
        char *self_color_owned = NULL;
        if (self_nick && self_nick[0])
        {
            self_color_owned = get_self_color(buffer_ptr);
            if (!self_color_owned)
                self_color_owned = xstrdup(get_nick_color_code(self_nick));
        }
        const char *body = NULL;
        char *prefix_str = extract_line_prefix(string, &body);
        char *colored_body = colorize_self_nick_only(body, self_nick,
                                                       self_color_owned ? self_color_owned : default_color,
                                                       "\x1C");
        dstr_t out;
        dstr_init(&out);
        dstr_append(&out, prefix_str);
        dstr_append(&out, colored_body);
        free(prefix_str);
        free(colored_body);
        free(self_color_owned);
        char *result = out.data;
        debug_hex(4, "irc_900 final_out", result);
        free(mdata_copy);
        return result;
    }
    if (is_315)
    {
        debugf(2, "irc_315 (End of /WHO list) - colorisation du canal "
                   "en tête si présent (buffer serveur)");
        const char *body = NULL;
        char *prefix_str = extract_line_prefix(string, &body);
        int len = (int)strlen(body);
        int pos = 0;
        while (pos < len)
        {
            int seqlen = match_control_seq(body, len, pos);
            if (seqlen == 0)
                break;
            pos += seqlen;
        }
        int channel_start = -1, channel_end = -1, after_pos = pos;
        char *channel_name = channel_enabled()
            ? detect_leading_channel(body, len, pos, &channel_start, &channel_end, &after_pos)
            : NULL;
        char *result;
        if (channel_name)
        {
            dstr_t out;
            dstr_init(&out);
            dstr_append(&out, prefix_str);
            /* Couleur de délimiteur standard de WeeChat (weechat.color.chat_delimiters,
             * ex. "brightgreen"), identique des deux côtés du crochet — pas
             * "default", qui ne correspond pas à cette couleur. */
            dstr_append(&out, weechat_color("chat_delimiters"));
            dstr_append_char(&out, '[');
            append_formatted_channel(&out, channel_name);
            dstr_append(&out, weechat_color("chat_delimiters"));
            dstr_append_char(&out, ']');
            dstr_append(&out, body + channel_end);
            result = out.data;
            free(channel_name);
        }
        else
        {
            result = xstrdup(string);
        }
        free(prefix_str);
        free(mdata_copy);
        return result;
    }
    if (is_352 || is_whois)
    {
        debugf(2, "Événement /who (irc_352) ou numérique whois - "
                   "colorisation du nick cible");
        struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
        if (!buffer)
        {
            free(mdata_copy);
            return NULL;
        }
        const char *srv = weechat_buffer_get_string(buffer, "localvar_server");
        const char *self_nick = srv ? weechat_info_get("irc_nick", srv) : NULL;
        const char *body = NULL;
        char *prefix_str = extract_line_prefix(string, &body);
        debug_hex(4, "who_whois raw_body", body);
        int bracket_start = -1, bracket_end = -1;
        char *channel_name = NULL;
        int channel_start = -1, channel_end = -1;
        char *target_nick = extract_target_nick(body, &bracket_start, &bracket_end,
                                                   &channel_name, &channel_start, &channel_end);
        debug_hex(4, "who_whois target_nick", target_nick);
        char *result;
        if (target_nick && target_nick[0])
        {
            char *target_color = NULL;
            if (self_nick && strcmp(target_nick, self_nick) == 0)
                target_color = get_self_color(buffer_ptr);
            if (!target_color)
                target_color = xstrdup(get_nick_color_code(target_nick));
            dstr_t out;
            dstr_init(&out);
            dstr_append(&out, prefix_str);
            /* Ligne imprimée sur le buffer serveur (WHO/whois lancé sur un
             * canal) : le nom du canal est dans le texte ("[#canal] nick ..."),
             * pas dans localvar_channel du buffer courant. On ne peut donc pas
             * réutiliser colorize_channel() ici ; on colore directement à la
             * position trouvée par extract_target_nick(). */
            const char *rest = body;
            if (channel_name && channel_name[0] && channel_enabled())
            {
                /* Couleur de délimiteur WeeChat (chat_delimiters), pas
                 * "default" — voir remarque équivalente dans irc_315. */
                dstr_append(&out, weechat_color("chat_delimiters"));
                dstr_append_char(&out, '[');
                append_formatted_channel(&out, channel_name);
                dstr_append(&out, weechat_color("chat_delimiters"));
                dstr_append_char(&out, ']');
                rest = body + channel_end;
            }
            else if (channel_name && channel_name[0])
            {
                dstr_append_n(&out, body, channel_end);
                rest = body + channel_end;
            }
            if (bracket_start >= 0)
            {
                /* Nick commençant littéralement par "[" (caractère valide en
                 * IRC), mutuellement exclusif avec channel_name. Même logique
                 * que pour les crochets de canal ci-dessus : couleur de
                 * délimiteur explicite et symétrique des deux côtés, au lieu
                 * de dupliquer deux fois le même fragment de "body" (bug
                 * pré-existant qui pouvait dupliquer du texte au lieu de
                 * simplement restaurer une couleur de délimiteur). */
                char *tail_colored = colorize_self_nick_only(body + bracket_end, target_nick,
                                                               target_color, "\x1C");
                dstr_append(&out, weechat_color("chat_delimiters"));
                dstr_append_char(&out, '[');
                append_formatted_nick(&out, NULL, target_nick, target_color);
                dstr_append(&out, weechat_color("chat_delimiters"));
                dstr_append_char(&out, ']');
                dstr_append(&out, tail_colored);
                free(tail_colored);
            }
            else
            {
                char *colored_body = colorize_self_nick_only(rest, target_nick,
                                                               target_color, "\x1C");
                dstr_append(&out, colored_body);
                free(colored_body);
            }
            result = out.data;
            free(target_color);
        }
        else
        {
            result = xstrdup(string);
        }
        debug_hex(4, "who_whois final_out", result);
        free(target_nick);
        free(channel_name);
        free(prefix_str);
        free(mdata_copy);
        return result;
    }
    if (is_324 || is_329 || is_353 || is_366)
    {
        debugf(2, "Événement IRC canal - colorisation du canal et des nicks");
        struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
        if (!buffer)
        {
            free(mdata_copy);
            return NULL;
        }
        char **all_nicks;
        int n_all;
        get_other_nicks(buffer_ptr, "", &all_nicks, &n_all);
        if (n_all > 0)
            sort_nicks_by_length_desc(all_nicks, n_all);
        markers_t markers;
        markers_init(&markers);
        char *clean = strip_control_codes(string, &markers);
        neutralize_midword_attr_markers(&clean, &markers);
        char *colored_clean = (n_all > 0)
            ? colorize_nicks_in_text(clean, all_nicks, n_all,
                                      NULL, "\x1C", 1, 0, &markers)
            : xstrdup(clean);
        free(clean);
        char *restored = restore_control_codes(colored_clean, &markers);
        free(colored_clean);
        markers_free(&markers);
        char *result = colorize_channel(restored, buffer_ptr, "\x1C");
        free(restored);
        debug_hex(4, "irc_324/329/353/366 final_out", result);
        for (int i = 0; i < n_all; i++)
            free(all_nicks[i]);
        free(all_nicks);
        free(mdata_copy);
        return result;
    }
    if (is_333)
    {
        char *result = process_333_event(string, buffer_ptr, link_color, sender);
        free(mdata_copy);
        return result;
    }
    if (is_332 || is_331 || is_topic_event)
    {
        debugf(2, "Événement topic : %s%s%s",
               is_332 ? "irc_332 " : "",
               is_331 ? "irc_331 " : "",
               is_topic_event ? "irc_topic" : "");
        debug_hex(4, "topic string_in", string);
        struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
        if (!buffer)
        {
            free(mdata_copy);
            return NULL;
        }
        char **all_nicks;
        int n_all;
        get_other_nicks(buffer_ptr, "", &all_nicks, &n_all);
        if (n_all > 0)
            sort_nicks_by_length_desc(all_nicks, n_all);
        markers_t markers;
        markers_init(&markers);
        char *clean = strip_control_codes(string, &markers);
        neutralize_midword_attr_markers(&clean, &markers);
        debug_hex(4, "topic clean", clean);
        if (is_topic_event)
            neutralize_quoted_colors(clean, &markers);
        char *colored_topic = NULL;
        if (n_all > 0)
        {
            colored_topic = colorize_nicks_in_text(clean, all_nicks, n_all,
                                                    NULL, "\x1C", 1, 0, &markers);
        }
        else
        {
            colored_topic = xstrdup(clean);
        }
        char *with_links = colorize_links_in_text(colored_topic, link_color, "\x1C", &markers);
        free(colored_topic);
        dstr_t out;
        dstr_init(&out);
        dstr_append(&out, with_links);
        free(with_links);
        char *restored = restore_control_codes(out.data, &markers);
        free(out.data);
        free(clean);
        markers_free(&markers);
        char *result = colorize_channel(restored, buffer_ptr, "\x1C");
        free(restored);
        debug_hex(4, "topic final_out", result);
        for (int i = 0; i < n_all; i++)
            free(all_nicks[i]);
        free(all_nicks);
        free(mdata_copy);
        return result;
    }
    int is_msg = is_privmsg || is_action || is_notice;
    int is_sys = is_join || is_part || is_quit || is_kick || is_nick_ev
                 || is_chghost || is_topic || is_account;
    line_counter++;
    current_lid = (int)line_counter;
    if (!is_msg && !is_mode && !is_sys)
    {
        current_lid = 0;
        free(mdata_copy);
        return NULL;
    }
    if (!sender[0])
    {
        debugf(2, "Pas de sender, retour ligne inchangée");
        current_lid = 0;
        free(mdata_copy);
        return NULL;
    }
    struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
    const char *server = buffer ? weechat_buffer_get_string(buffer, "localvar_server") : NULL;
    const char *self_nick_raw = (server && server[0]) ? weechat_info_get("irc_nick", server) : NULL;
    char *self_nick = self_nick_raw ? xstrdup(self_nick_raw) : NULL;
    char *sender_color = NULL;
    if (self_nick && strcmp(sender, self_nick) == 0)
        sender_color = get_self_color(buffer_ptr);
    if (!sender_color)
        sender_color = xstrdup(get_nick_color_code(sender));
    debugf(3, "buffer_search(\"==\", ...) = %s, server=%s, self_nick=%s",
           buffer ? "OK" : "NULL(!)", server ? server : "(null)",
           self_nick ? self_nick : "(null)");
    debug_hex(4, "sender", sender);
    debug_hex(4, "sender_color", sender_color);
    debug_hex(4, "string_in", string);
    if (is_join)
        add_nick_to_cache(buffer_ptr, sender);
    else if (is_part || is_quit || is_kick || is_nick_ev)
        invalidate_channel_cache_for(buffer_ptr);
    char *line = NULL;
    if (is_msg)
    {
        char *self_color_for_notice = NULL;
        if (is_notice && self_nick && self_nick[0])
        {
            self_color_for_notice = get_self_color(buffer_ptr);
            if (!self_color_for_notice)
                self_color_for_notice = xstrdup(get_nick_color_code(self_nick));
        }
        line = process_message(string, buffer_ptr, sender, sender_color, link_color,
                                (is_notice ? self_nick : NULL), self_color_for_notice);
        free(self_color_for_notice);
    }
    else if (is_mode)
        line = process_mode(string, buffer_ptr, sender, sender_color, link_color);
    else if (is_sys)
    {
        const char *event = is_join ? "join" : is_part ? "part" : is_quit ? "quit"
                             : is_kick ? "kick" : is_nick_ev ? "nick"
                             : is_chghost ? "chghost" : "account";
        line = process_system_event(string, sender, sender_color, event, buffer_ptr, link_color, self_nick);
    }
    char *final = colorize_channel(line ? line : string, buffer_ptr, sender_color);
    free(line);
    free(self_nick);
    debug_hex(4, "final_out", final);
    debugf(1, "%s traite", is_msg ? "Message" : is_mode ? "Mode" : "Système");
    free(sender_color);
    current_lid = 0;
    free(mdata_copy);
    return final;
}
static char *
modifier_cb(const void *pointer, void *data, const char *modifier,
            const char *modifier_data, const char *string)
{
    char *result;
    if (setjmp(oom_jmp))
    {
        oom_guard_active = 0;
        weechat_printf(NULL, "colorZ: mémoire insuffisante, ligne renvoyée "
                              "sans modification");
        return NULL;
    }
    oom_guard_active = 1;
    result = modifier_cb_impl(pointer, data, modifier, modifier_data, string);
    oom_guard_active = 0;
    return result;
}
static char *
input_modifier_cb_impl(const void *pointer, void *data, const char *modifier,
                        const char *modifier_data, const char *string)
{
    (void)pointer; (void)data; (void)modifier;
    if (!input_bar_enabled())
        return NULL;
    char *mdata_copy = xstrdup(modifier_data);
    char *semicolon = strchr(mdata_copy, ';');
    if (semicolon)
        *semicolon = '\0';
    char *buffer_ptr = mdata_copy;
    struct t_gui_buffer *buffer = resolve_buffer(buffer_ptr);
    if (!buffer)
    {
        free(mdata_copy);
        return NULL;
    }
    const char *channel = weechat_buffer_get_string(buffer, "localvar_channel");
    const char *server  = weechat_buffer_get_string(buffer, "localvar_server");
    if (!channel || !channel[0] || !server || !server[0])
    {
        free(mdata_copy);
        return NULL;
    }
    const char *self_nick = weechat_info_get("irc_nick", server);
    if (!self_nick || !self_nick[0])
    {
        free(mdata_copy);
        return NULL;
    }
    char *self_color = get_self_color(buffer_ptr);
    if (!self_color)
        self_color = xstrdup(get_nick_color_code(self_nick));
    char **all_nicks;
    int n_all;
    build_all_nicks_list(buffer_ptr, self_nick, &all_nicks, &n_all);
    debugf(3, "input_modifier: self_nick=%s, n_all_nicks=%d", self_nick, n_all);
    debug_hex(4, "input_string_in", string);
    char *with_nicks = colorize_nicks_input(string, all_nicks, n_all, self_nick,
                                             self_color, self_color, 0);
    const char *link_color_config = weechat_config_get_plugin("links_color");
    if (!link_color_config || !link_color_config[0])
        link_color_config = "white";
    char *with_links = colorize_links_in_text(with_nicks, link_color_config, self_color, NULL);
    free(with_nicks);
    char *result = with_links;
    if (channel_enabled()) {
        result = colorize_channel(with_links, buffer_ptr, self_color);
        free(with_links);
    }
    debug_hex(4, "input_result", result);
    free_nick_list(all_nicks, n_all);
    free(self_color);
    free(mdata_copy);
    return result;
}
static char *
input_modifier_cb(const void *pointer, void *data, const char *modifier,
                   const char *modifier_data, const char *string)
{
    char *result;
    if (setjmp(oom_jmp))
    {
        oom_guard_active = 0;
        weechat_printf(NULL, "colorZ: mémoire insuffisante, barre de saisie "
                              "renvoyée sans modification");
        return NULL;
    }
    oom_guard_active = 1;
    result = input_modifier_cb_impl(pointer, data, modifier, modifier_data, string);
    oom_guard_active = 0;
    return result;
}
int
weechat_plugin_init(struct t_weechat_plugin *plugin, int argc, char *argv[])
{
    (void)argc; (void)argv;
    weechat_plugin = plugin;
    const char *wee_dir = weechat_info_get("weechat_dir", "");
    if (wee_dir && wee_dir[0])
        snprintf(log_file_path, sizeof(log_file_path), "%s/colorZdebug.log", wee_dir);
    bold_on  = xstrdup(weechat_color("bold"));
    bold_off = xstrdup(weechat_color("-bold"));
    ital_on  = xstrdup(weechat_color("italic"));
    ital_off = xstrdup(weechat_color("-italic"));
    underline_on  = xstrdup(weechat_color("underline"));
    underline_off = xstrdup(weechat_color("-underline"));
    nick_color_cache = weechat_hashtable_new(32,
        WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING, NULL, NULL);
    channel_nicks_cache = weechat_hashtable_new(32,
        WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING, NULL, NULL);
    migrate_renamed_option("link_detect", "links");
    migrate_renamed_option("link_color", "links_color");
    migrate_renamed_option("link_underline", "links_underline");
    migrate_renamed_option("hosts", "hostmask_nicks");
    if (weechat_config_is_set_plugin("link_bold_italic"))
    {
        const char *old_val = weechat_config_get_plugin("link_bold_italic");
        if (!weechat_config_is_set_plugin("links_bold"))
            weechat_config_set_plugin("links_bold", old_val);
        if (!weechat_config_is_set_plugin("links_italic"))
            weechat_config_set_plugin("links_italic", old_val);
        weechat_config_unset_plugin("link_bold_italic");
    }
    if (weechat_config_is_set_plugin("channel_bold_italic"))
    {
        const char *old_val = weechat_config_get_plugin("channel_bold_italic");
        if (!weechat_config_is_set_plugin("channel_bold"))
            weechat_config_set_plugin("channel_bold", old_val);
        if (!weechat_config_is_set_plugin("channel_italic"))
            weechat_config_set_plugin("channel_italic", old_val);
        weechat_config_unset_plugin("channel_bold_italic");
    }
    if (weechat_config_is_set_plugin("nicks_bold_italic"))
    {
        const char *old_val = weechat_config_get_plugin("nicks_bold_italic");
        if (!weechat_config_is_set_plugin("nicks_bold"))
            weechat_config_set_plugin("nicks_bold", old_val);
        if (!weechat_config_is_set_plugin("nicks_italic"))
            weechat_config_set_plugin("nicks_italic", old_val);
        weechat_config_unset_plugin("nicks_bold_italic");
    }
    set_default("debug", "0", "Debug level (0-7)");
    set_default("channel", "on",
                "Activer/désactiver la mise en évidence du nom du canal (on/off)");
    set_default("channel_color", "white",
                "Couleur pour le nom du canal (ex: white, yellow, cyan, lightred, etc.)");
    set_default("channel_bold", "on",
                "Mettre le nom du canal en gras (on/off)");
    set_default("channel_italic", "on",
                "Mettre le nom du canal en italique (on/off)");
    set_default("lines", "on",
                "Activer le formatage des lignes (on/off)");
    set_default("hostmask_nicks", "on",
                "Coloriser les nicks (y compris dans le masque user@host) dans les "
                "événements join/part/quit (on/off)");
    set_default("hostmask_nicks_bold", "on",
                "Mettre en gras les nicks colorisés dans les événements "
                "join/part/quit (on/off)");
    set_default("hostmask_nicks_italic", "on",
                "Mettre en italique les nicks colorisés dans les événements "
                "join/part/quit (on/off)");
    set_default("events", "on",
                "Activer/désactiver le formatage des événements système : kick, "
                "changement de nick, chghost, account (couleur + nicks, gras/italique "
                "via events_bold/events_italic, indépendamment de nicks/nicks_bold/"
                "nicks_italic) ainsi que les réponses canal/topic (324, 329, 332, 333, "
                "353, 366, 221, 396 - pour ces dernières, la colorisation des nicks "
                "reste en plus soumise à l'option nicks). N'affecte ni les messages "
                "normaux (lines/nicks) ni join/part/quit (hostmask_nicks) (on/off)");
    set_default("events_bold", "on",
                "Mettre en gras les nicks colorisés dans les événements système "
                "kick/nick/chghost/account (on/off)");
    set_default("events_italic", "on",
                "Mettre en italique les nicks colorisés dans les événements système "
                "kick/nick/chghost/account (on/off)");
    set_default("nicks", "on",
                "Activer/désactiver la colorisation des nicks mentionnés dans le texte "
                "(messages, modes, topic, barre de saisie, et réponses canal 324/329/"
                "332/353/366) (on/off)");
    set_default("nicks_bold", "on",
                "Mettre les nicks en gras (on/off)");
    set_default("nicks_italic", "on",
                "Mettre les nicks en italique (on/off)");
    set_default("links_color", "white",
                "Couleur pour les liens (ex: white, yellow, cyan, lightred, etc.)");
    set_default("links_bold", "on",
                "Mettre les liens en gras (on/off)");
    set_default("links_italic", "on",
                "Mettre les liens en italique (on/off)");
    set_default("links_underline", "on",
                "Souligner les liens (on/off)");
    set_default("links", "on",
                "Activer/désactiver la détection et la colorisation des liens (on/off)");
    set_default("input_bar", "on",
                "Activer/désactiver le formatage des nicks et liens dans la barre de saisie (on/off)");
    refresh_options_cache();
    weechat_hook_config("plugins.var.colorZ.*", &config_changed_cb, NULL, NULL);
    weechat_hook_command("colorZdebug", "Régler le niveau de debug",
                          "0|1|2|3|4|5|6|7",
                          "0=off 1=simple 2=verbeux 3=très verbeux 4=dump ligne "
                          "5=hex dump 6=détail 7=segments",
                          "0|1|2|3|4|5|6|7", &cmd_colorZdebug_cb, NULL, NULL);
    weechat_hook_signal("*,irc_in_join", &nicklist_changed_cb, NULL, NULL);
    weechat_hook_signal("*,irc_in_part", &nicklist_changed_cb, NULL, NULL);
    weechat_hook_signal("*,irc_in_quit", &nicklist_changed_cb, NULL, NULL);
    weechat_hook_signal("*,irc_in_kick", &nicklist_changed_cb, NULL, NULL);
    weechat_hook_signal("*,irc_in_nick", &nicklist_changed_cb, NULL, NULL);
    weechat_hook_signal("*,irc_in_366", &nicklist_changed_cb, NULL, NULL);
    weechat_hook_signal("*,irc_in_333", &irc_in_333_signal_cb, NULL, NULL);
    weechat_hook_modifier("weechat_print", &modifier_cb, NULL, NULL);
    weechat_hook_modifier("input_text_display", &input_modifier_cb, NULL, NULL);
    debugf(1, "colorZ (plugin C) chargé - version 1.1 (debug=%d)", g_opts.debug);
    return WEECHAT_RC_OK;
}
int
weechat_plugin_end(struct t_weechat_plugin *plugin)
{
    (void)plugin;
    debugf(1, "Arrêt de colorZ");
    weechat_hashtable_free(nick_color_cache);
    weechat_hashtable_free(channel_nicks_cache);
    nick_cache_untrack_all();
    channel_cache_untrack_all();
    free(bold_on);
    free(bold_off);
    free(ital_on);
    free(ital_off);
    free(underline_on);
    free(underline_off);
    return WEECHAT_RC_OK;
}
