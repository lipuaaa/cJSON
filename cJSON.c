/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/* cJSON */
/* JSON parser in C. */

/* cJSON核心注释：
 * 解析器核心职责：将JSON字符串转换为cJSON树形数据结构；
 * 生成器核心职责：将cJSON树形结构转回格式化/非格式化JSON字符串。
 */

/* 禁用MSVC对C89旧函数的警告（如strdup、malloc等） */
#if !defined(_CRT_SECURE_NO_DEPRECATE) && defined(_MSC_VER)
#define _CRT_SECURE_NO_DEPRECATE
#endif

/* GCC编译器下，设置符号默认可见性为public（动态库导出用） */
#ifdef __GNUC__
#pragma GCC visibility push(default)
#endif
/* MSVC编译器下，禁用“系统头文件中单行注释”的警告（C89兼容） */
#if defined(_MSC_VER)
#pragma warning (push)
/* disable warning about single line comments in system headers */
#pragma warning (disable : 4001)
#endif

/* 解析器依赖的核心头文件：字符串、IO、数学、内存、字符处理等 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <float.h>

/* 可选：启用本地化支持（影响数值解析的小数点符号） */
#ifdef ENABLE_LOCALES
#include <locale.h>
#endif

/* 恢复MSVC警告级别 */
#if defined(_MSC_VER)
#pragma warning (pop)
#endif
#ifdef __GNUC__
#pragma GCC visibility pop
#endif

/* 引入cJSON核心头文件（定义数据结构、宏、函数声明） */
#include "cJSON.h"

/* ========== 解析器基础类型/宏定义 ========== */
/* 重新定义布尔类型（避免与系统宏冲突），适配cJSON的布尔逻辑 */
#ifdef true
#undef true
#endif
#define true ((cJSON_bool)1)

#ifdef false
#undef false
#endif
#define false ((cJSON_bool)0)

/* 兼容ANSI C（C89）：定义isinf/isnan宏（C99及以上math.h已内置）
 * isinf：判断是否为无穷大（通过d-d为NaN且d本身非NaN判定）
 * isnan：判断是否为非数值（NaN，通过d != d判定，NaN的特性）
 */
#ifndef isinf
#define isinf(d) (isnan((d - d)) && !isnan(d))
#endif
#ifndef isnan
#define isnan(d) (d != d)
#endif

/* 兼容定义NaN值（不同平台实现差异） */
#ifndef NAN
#ifdef _WIN32
#define NAN sqrt(-1.0)   /* Windows下通过负数开平方生成NaN */
#else
#define NAN 0.0/0.0      /* 类Unix下通过0除0生成NaN */
#endif
#endif

/* ========== 解析器错误处理核心结构 ========== */
/* 全局错误信息结构体：记录解析失败时的JSON字符串指针和偏移位置
 * 作用：解析出错时，通过cJSON_GetErrorPtr()返回错误位置
 */
typedef struct {
    const unsigned char *json;    /* 待解析的JSON原字符串 */
    size_t position;              /* 解析出错时的字符偏移量 */
} error;
static error global_error = { NULL, 0 };  /* 全局错误实例，初始化为空 */

/* 获取解析错误位置的公共接口
 * 返回值：解析失败时，指向JSON字符串中错误位置的指针；成功时为NULL
 */
CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void)
{
    return (const char*) (global_error.json + global_error.position);
}

/* ========== 解析结果取值辅助函数 ========== */
/* 获取JSON字符串类型节点的字符串值
 * 参数：item - 待取值的cJSON节点
 * 返回值：成功返回字符串指针；失败（非字符串类型）返回NULL
 */
CJSON_PUBLIC(char *) cJSON_GetStringValue(const cJSON * const item)
{
    if (!cJSON_IsString(item))
    {
        return NULL;
    }

    return item->valuestring;
}

/* 获取JSON数值类型节点的数值（double型）
 * 参数：item - 待取值的cJSON节点
 * 返回值：成功返回double数值；失败（非数值类型）返回NaN
 */
CJSON_PUBLIC(double) cJSON_GetNumberValue(const cJSON * const item)
{
    if (!cJSON_IsNumber(item))
    {
        return (double) NAN;
    }

    return item->valuedouble;
}

/* 版本一致性校验：防止C文件和头文件版本不匹配导致的解析异常
 * 编译期检查：如果cJSON.h和cJSON.c的版本号不一致，直接报编译错误
 */
#if (CJSON_VERSION_MAJOR != 1) || (CJSON_VERSION_MINOR != 7) || (CJSON_VERSION_PATCH != 19)
    #error cJSON.h and cJSON.c have different versions. Make sure that both have the same.
#endif

/* 获取cJSON版本号的公共接口
 * 返回值：格式为"x.y.z"的版本字符串（静态内存，无需释放）
 */
CJSON_PUBLIC(const char*) cJSON_Version(void)
{
    static char version[15];
    sprintf(version, "%i.%i.%i", CJSON_VERSION_MAJOR, CJSON_VERSION_MINOR, CJSON_VERSION_PATCH);

    return version;
}

/* ========== 解析器辅助工具函数：字符串比较 ========== */
/* 大小写不敏感的字符串比较（解析JSON对象key时的核心逻辑）
 * 参数：string1/string2 - 待比较的两个字符串
 * 返回值：0=相等；非0=不等（差值）；若任一字符串为NULL，返回1（视为不等）
 * 注：JSON标准中key是大小写敏感的，但cJSON早期版本默认不敏感，需用CaseSensitive接口
 */
static int case_insensitive_strcmp(const unsigned char *string1, const unsigned char *string2)
{
    if ((string1 == NULL) || (string2 == NULL))
    {
        return 1;
    }

    if (string1 == string2)
    {
        return 0;
    }

    /* 逐字符转换为小写比较，直到遇到不同字符或字符串结束 */
    for(; tolower(*string1) == tolower(*string2); (void)string1++, string2++)
    {
        if (*string1 == '\0')
        {
            return 0;
        }
    }

    return tolower(*string1) - tolower(*string2);
}

/* ========== 解析器内存管理核心结构 ========== */
/* 内存钩子结构体：自定义内存分配/释放/重分配函数
 * 作用：允许用户替换cJSON默认的malloc/free/realloc，适配嵌入式等定制场景
 */
typedef struct internal_hooks
{
    void *(CJSON_CDECL *allocate)(size_t size);                  /* 内存分配函数（替代malloc） */
    void (CJSON_CDECL *deallocate)(void *pointer);               /* 内存释放函数（替代free） */
    void *(CJSON_CDECL *reallocate)(void *pointer, size_t size); /* 内存重分配（替代realloc） */
} internal_hooks;

/* MSVC编译器兼容：避免dllimport符号的地址非静态问题
 * 封装默认内存函数为静态函数，确保符号可访问
 */
#if defined(_MSC_VER)
/* work around MSVC error C2322: '...' address of dllimport '...' is not static */
static void * CJSON_CDECL internal_malloc(size_t size)
{
    return malloc(size);
}
static void CJSON_CDECL internal_free(void *pointer)
{
    free(pointer);
}
static void * CJSON_CDECL internal_realloc(void *pointer, size_t size)
{
    return realloc(pointer, size);
}
#else
/* 非MSVC编译器：直接映射到系统默认内存函数 */
#define internal_malloc malloc
#define internal_free free
#define internal_realloc realloc
#endif

/* 编译期计算字符串字面量长度（避免运行期strlen） */
/* strlen of character literals resolved at compile time */
#define static_strlen(string_literal) (sizeof(string_literal) - sizeof(""))

/* 全局内存钩子实例：默认使用系统malloc/free/realloc */
static internal_hooks global_hooks = { internal_malloc, internal_free, internal_realloc };

/* 字符串拷贝函数（适配自定义内存钩子）
 * 参数：string - 源字符串；hooks - 内存钩子（指定分配函数）
 * 返回值：成功返回拷贝后的字符串（需通过hooks->deallocate释放）；失败返回NULL
 */
static unsigned char* cJSON_strdup(const unsigned char* string, const internal_hooks * const hooks)
{
    size_t length = 0;
    unsigned char *copy = NULL;

    if (string == NULL)
    {
        return NULL;
    }

    /* 计算字符串长度（含终止符'\0'） */
    length = strlen((const char*)string) + sizeof("");
    /* 用自定义内存分配函数分配空间 */
    copy = (unsigned char*)hooks->allocate(length);
    if (copy == NULL)
    {
        return NULL;
    }
    memcpy(copy, string, length);  /* 内存拷贝（含终止符） */

    return copy;
}

/* 初始化cJSON内存钩子的公共接口
 * 参数：hooks - 用户自定义的内存钩子（NULL表示恢复默认）
 * 逻辑：
 * 1. 若hooks为NULL，恢复为系统默认malloc/free/realloc；
 * 2. 若hooks非空，替换为用户自定义的malloc/free；
 * 3. 仅当malloc/free均为系统默认时，才启用realloc（避免自定义内存池冲突）
 */
CJSON_PUBLIC(void) cJSON_InitHooks(cJSON_Hooks* hooks)
{
    if (hooks == NULL)
    {
        /* 重置为默认内存函数 */
        global_hooks.allocate = malloc;
        global_hooks.deallocate = free;
        global_hooks.reallocate = realloc;
        return;
    }

    /* 初始化分配函数：默认用malloc，用户有自定义则替换 */
    global_hooks.allocate = malloc;
    if (hooks->malloc_fn != NULL)
    {
        global_hooks.allocate = hooks->malloc_fn;
    }

    /* 初始化释放函数：默认用free，用户有自定义则替换 */
    global_hooks.deallocate = free;
    if (hooks->free_fn != NULL)
    {
        global_hooks.deallocate = hooks->free_fn;
    }

    /* 重分配函数：仅当分配/释放均为系统默认时才启用（避免自定义内存池不兼容） */
    global_hooks.reallocate = NULL;
    if ((global_hooks.allocate == malloc) && (global_hooks.deallocate == free))
    {
        global_hooks.reallocate = realloc;
    }
}

/* ========== 解析器核心：cJSON节点创建/销毁 ========== */
/* 内部节点构造函数：创建空的cJSON节点（解析器构建树形结构的基础）
 * 参数：hooks - 内存钩子（指定分配函数）
 * 返回值：成功返回cJSON节点指针；失败（内存不足）返回NULL
 * 逻辑：分配内存后用memset清零，确保所有字段初始化为空
 */
/* Internal constructor. */
static cJSON *cJSON_New_Item(const internal_hooks * const hooks)
{
    cJSON* node = (cJSON*)hooks->allocate(sizeof(cJSON));
    if (node)
    {
        memset(node, '\0', sizeof(cJSON));
    }

    return node;
}

/* 销毁cJSON节点（及所有子节点）的公共接口（解析完成后释放内存的核心）
 * 参数：item - 待销毁的cJSON根节点
 * 逻辑：
 * 1. 遍历链表（next指针），逐个销毁节点；
 * 2. 若节点非引用类型：销毁子节点、释放字符串值、释放key字符串；
 * 3. 最后释放当前节点内存；
 */
/* Delete a cJSON structure. */
CJSON_PUBLIC(void) cJSON_Delete(cJSON *item)
{
    cJSON *next = NULL;
    while (item != NULL)
    {
        next = item->next;  /* 先保存下一个节点，避免销毁后指针失效 */
        /* 非引用类型 + 有子节点：递归销毁子节点（数组/对象的子元素） */
        if (!(item->type & cJSON_IsReference) && (item->child != NULL))
        {
            cJSON_Delete(item->child);
        }
        /* 非引用类型 + 有字符串值：释放valuestring（字符串/原始JSON值） */
        if (!(item->type & cJSON_IsReference) && (item->valuestring != NULL))
        {
            global_hooks.deallocate(item->valuestring);
            item->valuestring = NULL;
        }
        /* 非常量字符串 + 有key字符串：释放object的key字符串 */
        if (!(item->type & cJSON_StringIsConst) && (item->string != NULL))
        {
            global_hooks.deallocate(item->string);
            item->string = NULL;
        }
        /* 释放当前节点内存 */
        global_hooks.deallocate(item);
        item = next;
    }
}

/* ========== 数值解析辅助：获取本地化小数点符号 ========== */
/* 获取当前系统本地化设置的小数点符号（影响数值解析的兼容性）
 * 返回值：默认返回'.'；启用ENABLE_LOCALES时返回系统本地化小数点（如','）
 */
/* get the decimal point character of the current locale */
static unsigned char get_decimal_point(void)
{
#ifdef ENABLE_LOCALES
    struct lconv *lconv = localeconv();
    return (unsigned char) lconv->decimal_point[0];
#else
    return '.';
#endif
}

/* ========== 解析器核心：解析缓冲区结构体 ========== */
/* 解析缓冲区结构体：封装待解析的JSON字符串、偏移量、嵌套深度等信息
 * 作用：统一管理解析过程中的上下文，简化参数传递
 */
typedef struct
{
    const unsigned char *content;  /* 待解析的JSON原始字符串 */
    size_t length;                 /* JSON字符串总长度 */
    size_t offset;                 /* 当前解析的字符偏移量（进度） */
    size_t depth;                  /* 当前解析的嵌套深度（数组/对象），防止栈溢出 */
    internal_hooks hooks;          /* 内存钩子（解析过程中分配节点用） */
} parse_buffer;

/* 解析缓冲区宏：简化边界检查（核心安全逻辑，防止越界访问） */
/* 检查是否能读取size个字符（从当前offset开始，1-based） */
#define can_read(buffer, size) ((buffer != NULL) && (((buffer)->offset + size) <= (buffer)->length))
/* 检查是否能访问指定索引的字符（0-based） */
#define can_access_at_index(buffer, index) ((buffer != NULL) && (((buffer)->offset + index) < (buffer)->length))
#define cannot_access_at_index(buffer, index) (!can_access_at_index(buffer, index))
/* 获取当前offset位置的字符指针 */
#define buffer_at_offset(buffer) ((buffer)->content + (buffer)->offset)

/* ========== 解析器核心：数值解析函数（JSON Number类型） ========== */
/* 解析JSON数值字符串，填充到cJSON节点中
 * 参数：
 *   item - 待填充的cJSON节点（需提前创建）
 *   input_buffer - 解析缓冲区（含JSON字符串、偏移量等）
 * 返回值：true=解析成功；false=解析失败（内存不足/格式非法）
 * 核心逻辑：
 * 1. 提取数值字符串片段（0-9/+/-/./e/E）；
 * 2. 适配本地化小数点（将'.'替换为系统小数点）；
 * 3. 用strtod转换为double值，填充到节点；
 * 4. 更新解析缓冲区偏移量，完成数值解析；
 */
/* Parse the input text to generate a number, and populate the result into item. */
static cJSON_bool parse_number(cJSON * const item, parse_buffer * const input_buffer)
{
    double number = 0;
    unsigned char *after_end = NULL;
    unsigned char *number_c_string;
    unsigned char decimal_point = get_decimal_point();  /* 获取系统小数点符号 */
    size_t i = 0;
    size_t number_string_length = 0;
    cJSON_bool has_decimal_point = false;  /* 标记是否包含小数点 */

    /* 入参合法性检查：缓冲区/内容为空则失败 */
    if ((input_buffer == NULL) || (input_buffer->content == NULL))
    {
        return false;
    }

    /* 第一步：提取数值字符串片段（遍历直到非数值字符）
     * 支持的字符：0-9、+、-、e/E、.（小数点）
     */
    /* copy the number into a temporary buffer and replace '.' with the decimal point
     * of the current locale (for strtod)
     * This also takes care of '\0' not necessarily being available for marking the end of the input */
    for (i = 0; can_access_at_index(input_buffer, i); i++)
    {
        switch (buffer_at_offset(input_buffer)[i])
        {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            case '+':
            case '-':
            case 'e':
            case 'E':
                number_string_length++;/* 合法数值字符，长度+1 */
                break;

            case '.':
                number_string_length++;
                has_decimal_point = true;/* 标记包含小数点 */
                break;

            default:
                goto loop_end;/* 遇到非数值字符，终止遍历 */
        }
    }
loop_end:
    /* 第二步：分配临时缓冲区存储数值字符串（+1为终止符'\0'） */
    number_c_string = (unsigned char *) input_buffer->hooks.allocate(number_string_length + 1);
    if (number_c_string == NULL)
    {
        return false; /* 内存分配失败，解析失败 */
    }

    /* 拷贝数值字符串到临时缓冲区，并添加终止符 */
    memcpy(number_c_string, buffer_at_offset(input_buffer), number_string_length);
    number_c_string[number_string_length] = '\0';

    /* 第三步：适配本地化小数点（将字符串中的'.'替换为系统小数点） */
    if (has_decimal_point)
    {
        for (i = 0; i < number_string_length; i++)
        {
            if (number_c_string[i] == '.')
            {
                /* replace '.' with the decimal point of the current locale (for strtod) */
                number_c_string[i] = decimal_point;
            }
        }
    }

    /* 第四步：将字符串转换为double数值（strtod适配本地化小数点） */
    number = strtod((const char*)number_c_string, (char**)&after_end);
    /* 检查转换是否成功：若转换后指针未到字符串末尾，说明格式非法 */
    if (number_c_string == after_end)
    {
        /* free the temporary buffer */
        input_buffer->hooks.deallocate(number_c_string);
        return false; /* parse_error */
    }

    /* 第五步：填充cJSON节点的数值字段 */
    item->valuedouble = number;

    /* use saturation in case of overflow */
    if (number >= INT_MAX)
    {
        item->valueint = INT_MAX;
    }
    else if (number <= (double)INT_MIN)
    {
        item->valueint = INT_MIN;
    }
    else
    {
        item->valueint = (int)number;
    }

    item->type = cJSON_Number;

    /* 第六步：更新解析缓冲区偏移量（跳过已解析的数值字符串） */
    input_buffer->offset += (size_t)(after_end - number_c_string);
    /* 释放临时缓冲区，返回解析成功 */
    input_buffer->hooks.deallocate(number_c_string);
    return true;
}

/* 数值设置辅助函数（兼容旧版接口）
 * 注释说明：原cJSON_SetNumberValue设计不规范，同时返回int/double，此函数统一返回double
 * 参数：object - 待设置的cJSON节点；number - 要设置的double数值
 * 返回值：设置后的double值（方便链式调用）
 * 核心逻辑：和parse_number一致，做int类型的溢出饱和保护
 */
/* don't ask me, but the original cJSON_SetNumberValue returns an integer or double */
CJSON_PUBLIC(double) cJSON_SetNumberHelper(cJSON *object, double number)
{
    if (number >= INT_MAX)
    {
        object->valueint = INT_MAX;
    }
    else if (number <= (double)INT_MIN)
    {
        object->valueint = INT_MIN;
    }
    else
    {
        object->valueint = (int)number;
    }
    
    /* 先设置valuedouble，再返回该值（赋值表达式的返回值是右值） */
    return object->valuedouble = number;
}

/* 设置cJSON字符串节点的valuestring（核心写操作接口）
 * 注意：传入NULL valuestring视为错误，返回NULL；
 *       仅允许修改非引用类型、字符串类型的节点valuestring
 * 参数：object - 目标cJSON节点；valuestring - 新的字符串值
 * 返回值：成功返回新的valuestring指针；失败返回NULL
 */
/* Note: when passing a NULL valuestring, cJSON_SetValuestring treats this as an error and return NULL */
CJSON_PUBLIC(char*) cJSON_SetValuestring(cJSON *object, const char *valuestring)
{
    char *copy = NULL;
    size_t v1_len;
    size_t v2_len;
    /* 合法性检查1：
     * - 节点为NULL → 非法
     * - 节点类型不是字符串 → 非法（仅字符串节点有valuestring）
     * - 节点是引用类型 → 非法（引用节点不允许修改valuestring）
     */
    /* if object's type is not cJSON_String or is cJSON_IsReference, it should not set valuestring */
    if ((object == NULL) || !(object->type & cJSON_String) || (object->type & cJSON_IsReference))
    {
        return NULL;
    }
    /* 合法性检查2：
     * - 原valuestring为NULL（节点损坏） → 非法
     * - 新valuestring为NULL → 非法（视为错误输入）
     */
    /* return NULL if the object is corrupted or valuestring is NULL */
    if (object->valuestring == NULL || valuestring == NULL)
    {
        return NULL;
    }

    v1_len = strlen(valuestring);
    v2_len = strlen(object->valuestring);

    if (v1_len <= v2_len)
    {
        /* 新字符串更短/等长：尝试直接覆盖（避免重新分配内存）
         * 关键检查：字符串重叠判断（strcpy不处理重叠内存，会导致数据错乱）
         * 重叠条件：新字符串的末尾 ≥ 原字符串起始，且原字符串末尾 ≥ 新字符串起始
         * 举例：valuestring="abc", object->valuestring="bc" → 重叠，禁止覆盖
         */
        /* strcpy does not handle overlapping string: [X1, X2] [Y1, Y2] => X2 < Y1 or Y2 < X1 */
        if (!( valuestring + v1_len < object->valuestring || object->valuestring + v2_len < valuestring ))
        {
            return NULL;
        }
        /* 无重叠，直接拷贝新字符串到原缓冲区（strcpy自动添加终止符） */
        strcpy(object->valuestring, valuestring);
        return object->valuestring;
    }
    /* 新字符串更长：需要重新分配内存 */
    copy = (char*) cJSON_strdup((const unsigned char*)valuestring, &global_hooks);
    if (copy == NULL)
    {
        return NULL;
    }
    /* 释放原valuestring内存（避免内存泄漏） */
    if (object->valuestring != NULL)
    {
        cJSON_free(object->valuestring);
    }
    /* 替换为新字符串指针 */
    object->valuestring = copy;

    return copy;
}

/* 打印缓冲区结构体（JSON生成器核心：将cJSON节点转为字符串的缓冲区）
 * 作用：封装字符串拼接的内存管理，支持动态扩容、格式化输出（缩进）
 */
typedef struct
{
    unsigned char *buffer;/* 存储生成的JSON字符串的缓冲区 */
    size_t length;        /* 缓冲区总长度（已分配的内存大小）*/
    size_t offset;        /* 当前写入位置的偏移量（已使用的长度）*/
    size_t depth;         /* 当前嵌套深度（用于格式化输出的缩进） */
    cJSON_bool noalloc;   /* 是否禁止内存分配（true=仅使用现有缓冲区，false=动态扩容）*/
    cJSON_bool format;    /* 是否格式化输出（true=带缩进/换行，false=紧凑格式）*/
    internal_hooks hooks; /* 内存钩子（缓冲区扩容时的分配/释放函数）*/
} printbuffer;

/* 确保打印缓冲区有足够空间（核心内存管理函数）
 * 参数：p - 打印缓冲区；needed - 需要新增的字节数（含终止符）
 * 返回值：成功返回缓冲区当前写入位置的指针；失败返回NULL
 * 核心逻辑：
 * 1. 检查入参合法性 2. 计算所需总空间 3. 空间足够直接返回 4. 空间不足则扩容
 */
/* realloc printbuffer if necessary to have at least "needed" bytes more */
static unsigned char* ensure(printbuffer * const p, size_t needed)
{
    unsigned char *newbuffer = NULL;
    size_t newsize = 0;

    /* 入参合法性检查：缓冲区/缓冲区指针为空 → 失败 */
    if ((p == NULL) || (p->buffer == NULL))
    {
        return NULL;
    }
    /* 偏移量越界检查：已使用长度 ≥ 总长度 → 失败 */
    if ((p->length > 0) && (p->offset >= p->length))
    {
        /* make sure that offset is valid */
        return NULL;
    }
    /* 长度超限检查：需要的字节数超过INT_MAX → 不支持，失败 */
    if (needed > INT_MAX)
    {
        /* sizes bigger than INT_MAX are currently not supported */
        return NULL;
    }
    /* 计算需要的总空间：已使用长度 + 新增长度 + 1（终止符） */
    needed += p->offset + 1;
    if (needed <= p->length)
    {
        return p->buffer + p->offset;
    }
    /* 禁止分配内存：直接返回失败（noalloc=true时，仅使用现有缓冲区） */
    if (p->noalloc) {
        return NULL;
    }

    /* 计算新缓冲区大小（扩容策略） */
    if (needed > (INT_MAX / 2))
    {
        /* 避免int溢出：若所需空间 ≤ INT_MAX，设为INT_MAX；否则失败 */
        if (needed <= INT_MAX)
        {
            newsize = INT_MAX;
        }
        else
        {
            return NULL;
        }
    }
    else
    {
        /* 常规扩容：新大小 = 所需空间 × 2（减少频繁扩容） */
        newsize = needed * 2;
    }
    /* 尝试扩容缓冲区 */
    if (p->hooks.reallocate != NULL)
    {
        /* 有realloc函数：直接重分配（效率更高，无需拷贝） */
        newbuffer = (unsigned char*)p->hooks.reallocate(p->buffer, newsize);
        if (newbuffer == NULL)
        {
            /* 重分配失败：释放原缓冲区，重置状态，返回NULL */            
            p->hooks.deallocate(p->buffer);
            p->length = 0;
            p->buffer = NULL;

            return NULL;
        }
    }
    else
    {
        /* 无realloc函数：手动分配新缓冲区 + 拷贝数据 + 释放原缓冲区 */
        newbuffer = (unsigned char*)p->hooks.allocate(newsize);
        if (!newbuffer)
        {
            /* 分配失败：释放原缓冲区，重置状态，返回NULL */
            p->hooks.deallocate(p->buffer);
            p->length = 0;
            p->buffer = NULL;

            return NULL;
        }
        /* 拷贝原缓冲区数据到新缓冲区（含终止符） */
        memcpy(newbuffer, p->buffer, p->offset + 1);
        /* 释放原缓冲区 */       
        p->hooks.deallocate(p->buffer);
    }
    /* 更新缓冲区状态：新长度、新指针 */
    p->length = newsize;
    p->buffer = newbuffer;
    /* 返回新缓冲区的当前写入位置指针 */
    return newbuffer + p->offset;
}

/* 更新打印缓冲区的偏移量（核心辅助函数）
 * 原理：计算当前写入位置到终止符的长度，更新offset为总已使用长度
 * 作用：确保后续写入不会覆盖已有数据，正确计算缓冲区使用量
 */
/* calculate the new length of the string in a printbuffer and update the offset */
static void update_offset(printbuffer * const buffer)
{
    const unsigned char *buffer_pointer = NULL;
    if ((buffer == NULL) || (buffer->buffer == NULL))
    {
        return;
    }
    buffer_pointer = buffer->buffer + buffer->offset;
    /* 更新offset：原有偏移 + 当前字符串长度（strlen自动忽略终止符） */
    buffer->offset += strlen((const char*)buffer_pointer);
}

/* 浮点数安全比较函数（解决浮点数精度问题）
 * 参数：a/b - 待比较的两个double值
 * 返回值：true=相等；false=不相等
 * 核心逻辑：
 * 1. 取两个数的绝对值最大值作为基准；
 * 2. 比较两数差值的绝对值 ≤ 基准 × DBL_EPSILON（双精度浮点数最小精度）；
 * 3. 避免直接用"=="比较浮点数（如0.1+0.2≠0.3）
 */
static cJSON_bool compare_double(double a, double b)
{
    double maxVal = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return (fabs(a - b) <= maxVal * DBL_EPSILON);
}

/* 将cJSON数值节点渲染为JSON字符串（生成器核心函数）
 * 参数：item - 数值类型的cJSON节点；output_buffer - 打印缓冲区
 * 返回值：true=渲染成功；false=失败
 * 核心逻辑：
 * 1. 处理NaN/无穷大 → 输出"null"（JSON标准不支持NaN/Infinity）；
 * 2. 整数型double → 输出int格式（更紧凑）；
 * 3. 浮点型double → 先尝试15位精度，验证是否可逆，不可逆则用17位；
 * 4. 适配本地化小数点 → 统一转为JSON标准的"."；
 */
static cJSON_bool print_number(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;/*缓冲区写入位置指针*/
    double d = item->valuedouble;/*节点的double值*/
    int length = 0;/*数值字符串长度*/
    size_t i = 0;/*循环索引*/
    unsigned char number_buffer[26] = {0}; /* 临时缓冲区：存储格式化后的数值字符串（26字节足够存储double的最大长度） */
    unsigned char decimal_point = get_decimal_point();/*本地化小数点符号*/
    double test = 0.0;/*验证转换可逆性的临时变量*/

    /* 入参合法性检查：缓冲区为空 → 失败 */
    if (output_buffer == NULL)
    {
        return false;
    }

    /* 处理NaN/无穷大：JSON标准无此类型，输出"null" */
    if (isnan(d) || isinf(d))
    {
        length = sprintf((char*)number_buffer, "null");
    }
    /* 处理整数型double：d和其int值相等（无小数部分），输出int格式 */
    else if(d == (double)item->valueint)
    {
        length = sprintf((char*)number_buffer, "%d", item->valueint);
    }
    /* 处理浮点型double：高精度输出并验证可逆性 */
    else
    {
        /* 第一步：尝试15位精度输出（%1.15g自动去除末尾无意义的0）*/
        length = sprintf((char*)number_buffer, "%1.15g", d);

        /* 验证转换可逆性：将输出的字符串转回double，比较是否和原值一致 */
        if ((sscanf((char*)number_buffer, "%lg", &test) != 1) || !compare_double((double)test, d))
        {
            /* 不可逆：用17位精度重新输出（double的最大有效精度） */
            length = sprintf((char*)number_buffer, "%1.17g", d);
        }
    }

    /* 错误检查：sprintf失败（返回负数）或缓冲区溢出 → 失败 */
    if ((length < 0) || (length > (int)(sizeof(number_buffer) - 1)))
    {
        return false;
    }

    /* 确保打印缓冲区有足够空间（+1为终止符） */
    output_pointer = ensure(output_buffer, (size_t)length + sizeof(""));
    if (output_pointer == NULL)
    {
        return false;
    }

    /* 拷贝数值字符串到输出缓冲区，统一小数点为JSON标准的"." */
    for (i = 0; i < ((size_t)length); i++)
    {
        if (number_buffer[i] == decimal_point)
        {
            output_pointer[i] = '.';
            continue;
        }

        output_pointer[i] = number_buffer[i];
    }
    output_pointer[i] = '\0';
    /* 更新打印缓冲区偏移量（跳过已写入的数值字符串） */
    output_buffer->offset += (size_t)length;

    return true;
}

/* 解析4位十六进制数（JSON字符串转义字符解析核心函数）
 * 作用：解析\uXXXX格式的Unicode转义字符（如\u4e2d → 中）
 * 参数：input - 指向4位十六进制字符串的指针（如"4e2d"）
 * 返回值：成功返回解析后的无符号整数；失败返回0
 * 核心逻辑：逐字符解析0-9/A-F/a-f，转换为4字节十六进制数
 */
static unsigned parse_hex4(const unsigned char * const input)
{
    unsigned int h = 0;/*存储解析后的十六进制数*/
    size_t i = 0;/*循环索引（遍历4位字符）*/

    for (i = 0; i < 4; i++)
    {
        /* 解析单个十六进制字符 */
        if ((input[i] >= '0') && (input[i] <= '9'))
        {
            h += (unsigned int) input[i] - '0';
        }
        else if ((input[i] >= 'A') && (input[i] <= 'F'))
        {
            h += (unsigned int) 10 + input[i] - 'A';
        }
        else if ((input[i] >= 'a') && (input[i] <= 'f'))
        {
            h += (unsigned int) 10 + input[i] - 'a';
        }
        else /* 无效字符 */
        {
            return 0;
        }
        /* 移位：前3位字符需要左移4位（每一位占4bit），最后一位无需移位 */
        if (i < 3)
        {
            /* 左移4位，为下一位腾出空间 */
            h = h << 4;
        }
    }

    return h;
}

/* 将UTF-16转义字面量（\uXXXX 或 \uXXXX\uXXXX）转换为UTF-8编码
 * 背景：JSON字符串中的Unicode转义字符是UTF-16格式，需转为UTF-8存储/输出
 * 参数：
 *   input_pointer - 指向UTF-16转义序列起始（如"\u4e2d"的起始'\'）
 *   input_end - 输入字符串结束位置（防止越界）
 *   output_pointer - 输出缓冲区指针（存储转换后的UTF-8字符，会更新指针位置）
 * 返回值：成功返回消耗的输入字节数（6=单\uXXXX，12=双\uXXXX\uXXXX）；失败返回0
 */
/* converts a UTF-16 literal to UTF-8
 * A literal can be one or two sequences of the form \uXXXX */
static unsigned char utf16_literal_to_utf8(const unsigned char * const input_pointer, const unsigned char * const input_end, unsigned char **output_pointer)
{
    long unsigned int codepoint = 0;                    /*最终的Unicode码点（0-0x10FFFF）*/
    unsigned int first_code = 0;                        /*第一个UTF-16序列的数值（\uXXXX）*/
    const unsigned char *first_sequence = input_pointer;/*第一个\uXXXX的起始指针*/
    unsigned char utf8_length = 0;                      /*转换后的UTF-8字符字节数（1-4）*/
    unsigned char utf8_position = 0;                    /*UTF-8编码时的字节索引*/
    unsigned char sequence_length = 0;                  /*输入UTF-16序列的总字节数（6/12）*/
    unsigned char first_byte_mark = 0;                  /*UTF-8第一个字节的标记位（如0xE0=11100000）*/
    
    /* 合法性检查1：输入剩余长度不足6字节（\uXXXX至少需要6字节：\ + u + 4位16进制） */
    if ((input_end - first_sequence) < 6)
    {
        /* input ends unexpectedly */
        goto fail;
    }

    /* 解析第一个UTF-16序列（跳过"\u"，取后4位16进制数） */
    first_code = parse_hex4(first_sequence + 2);

    /* 合法性检查2：第一个序列不能是UTF-16低代理项（0xDC00-0xDFFF）
     * 原理：UTF-16代理对规则：高代理项（0xD800-0xDBFF）+ 低代理项（0xDC00-0xDFFF）
     * 单独的低代理项是非法的
     */
    if (((first_code >= 0xDC00) && (first_code <= 0xDFFF)))
    {
        goto fail;
    }

    /* 处理UTF-16代理对（编码超过0xFFFF的Unicode字符，需两个\uXXXX序列） */
    if ((first_code >= 0xD800) && (first_code <= 0xDBFF))
    {
        const unsigned char *second_sequence = first_sequence + 6;
        unsigned int second_code = 0;
        sequence_length = 12; /* \uXXXX\uXXXX */

        /* 合法性检查3：第二个序列剩余长度不足6字节 */
        if ((input_end - second_sequence) < 6)
        {
            /* input ends unexpectedly */
            goto fail;
        }

        /* 合法性检查4：第二个序列必须以"\u"开头 */
        if ((second_sequence[0] != '\\') || (second_sequence[1] != 'u'))
        {
            /* missing second half of the surrogate pair */
            goto fail;
        }

        /* 解析第二个UTF-16序列（低代理项） */
        second_code = parse_hex4(second_sequence + 2);
        /* 合法性检查5：第二个序列必须是低代理项（0xDC00-0xDFFF） */
        if ((second_code < 0xDC00) || (second_code > 0xDFFF))
        {
            /* invalid second half of the surrogate pair */
            goto fail;
        }


        /* 计算完整的Unicode码点（代理对转换公式）
         * 原理：代理对表示 0x10000 ~ 0x10FFFF 的字符，公式：
         * codepoint = 0x10000 + (高代理项 & 0x3FF) << 10 + (低代理项 & 0x3FF)
         */
        codepoint = 0x10000 + (((first_code & 0x3FF) << 10) | (second_code & 0x3FF));
    }
    else
    {
        /* 普通UTF-16序列（单\uXXXX，码点≤0xFFFF） */
        sequence_length = 6; /* \uXXXX */
        codepoint = first_code;
    }

    /* 将Unicode码点编码为UTF-8（JSON标准要求UTF-8编码）
     * UTF-8编码规则（最多4字节）：
     * - 0x00~0x7F → 1字节：0xxxxxxx
     * - 0x80~0x7FF → 2字节：110xxxxx 10xxxxxx
     * - 0x800~0xFFFF → 3字节：1110xxxx 10xxxxxx 10xxxxxx
     * - 0x10000~0x10FFFF → 4字节：11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
     */
    if (codepoint < 0x80)
    {
        /* ASCII字符（0-127），1字节编码 */
        utf8_length = 1;
    }
    else if (codepoint < 0x800)
    {
        /* 2字节UTF-8，首字节标记位0xC0（11000000） */
        utf8_length = 2;
        first_byte_mark = 0xC0; /* 11000000 */
    }
    else if (codepoint < 0x10000)
    {
        /* 3字节UTF-8，首字节标记位0xE0（11100000） */
        utf8_length = 3;
        first_byte_mark = 0xE0; /* 11100000 */
    }
    else if (codepoint <= 0x10FFFF)
    {
        /* 4字节UTF-8，首字节标记位0xF0（11110000） */
        utf8_length = 4;
        first_byte_mark = 0xF0; /* 11110000 */
    }
    else
    {
        /* 非法Unicode码点（超过0x10FFFF） */
        goto fail;
    }

    /* 编码为UTF-8字节序列（从后往前填充） */
    for (utf8_position = (unsigned char)(utf8_length - 1); utf8_position > 0; utf8_position--)
    {
        /* 后续字节固定格式：10xxxxxx（0x80-0xBF） */
        (*output_pointer)[utf8_position] = (unsigned char)((codepoint | 0x80) & 0xBF);
        codepoint >>= 6;
    }
    /* 编码第一个字节 */
    if (utf8_length > 1)
    {
        /*多字节：首字节 = 标记位 | 剩余码点（确保高位正确）*/
        (*output_pointer)[0] = (unsigned char)((codepoint | first_byte_mark) & 0xFF);
    }
    else
    {
        /*单字节（ASCII）：直接取低7位（0x7F）*/
        (*output_pointer)[0] = (unsigned char)(codepoint & 0x7F);
    }

    *output_pointer += utf8_length;

    /* 更新输出指针：跳过已写入的UTF-8字节 */
    return sequence_length;

fail:
    return 0;
}

/* 解析JSON字符串（核心解析函数）：将带转义的JSON字符串转为无转义的C字符串，填充到cJSON节点
 * 参数：
 *   item - 待填充的cJSON节点（解析后标记为cJSON_String类型）
 *   input_buffer - 解析缓冲区（含JSON字符串、偏移量等）
 * 返回值：true=解析成功；false=解析失败
 */
static cJSON_bool parse_string(cJSON * const item, parse_buffer * const input_buffer)
{
    const unsigned char *input_pointer = buffer_at_offset(input_buffer) + 1;/*输入指针：跳过开头的双引号（"），指向字符串内容起始*/
    const unsigned char *input_end = buffer_at_offset(input_buffer) + 1;/*输入结束指针：初始指向开头双引号后，后续遍历到结束双引号*/
    unsigned char *output_pointer = NULL;/*输出缓冲区写入指针*/
    unsigned char *output = NULL;/*输出缓冲区（存储无转义的字符串）*/

    /* 合法性检查1：当前字符不是双引号 → 不是JSON字符串 */
    if (buffer_at_offset(input_buffer)[0] != '\"')
    {
        goto fail;
    }

    {
        /* 预计算输出缓冲区大小（高估，避免频繁扩容） */
        size_t allocation_length = 0;/*需分配的缓冲区大小*/
        size_t skipped_bytes = 0;/*转义字符占用的额外字节数（如"\n"→1字节，原占2字节）*/
        /*遍历字符串，直到结束双引号或缓冲区末尾*/
        while (((size_t)(input_end - input_buffer->content) < input_buffer->length) && (*input_end != '\"'))
        {
            /* 遇到转义符\ */
            if (input_end[0] == '\\')
            {
                /*合法性检查：转义符是最后一个字符 → 非法*/
                if ((size_t)(input_end + 1 - input_buffer->content) >= input_buffer->length)
                {
                    /* prevent buffer overflow when last input character is a backslash */
                    goto fail;
                }
                skipped_bytes++;/*转义符会减少最终输出长度，记录跳过的字节*/
                input_end++;/*跳过转义符，指向转义字符（如\n的n）*/
            }
            input_end++;/*移动到下一个字符*/
        }
        /* 合法性检查2：遍历到缓冲区末尾仍未找到结束双引号 → 字符串未结束 */
        if (((size_t)(input_end - input_buffer->content) >= input_buffer->length) || (*input_end != '\"'))
        {
            goto fail; /* string ended unexpectedly */
        }

        /* 计算输出缓冲区大小：输入长度 - 跳过的转义字节 + 终止符 */
        allocation_length = (size_t) (input_end - buffer_at_offset(input_buffer)) - skipped_bytes;
        output = (unsigned char*)input_buffer->hooks.allocate(allocation_length + sizeof(""));
        if (output == NULL)
        {
            goto fail; /* 内存分配失败 */
        }
    }

    output_pointer = output;
    /* 遍历字符串内容，处理转义并写入输出缓冲区 */
    while (input_pointer < input_end)
    {
        /* 普通字符（非转义符）：直接拷贝 */
        if (*input_pointer != '\\')
        {
            *output_pointer++ = *input_pointer++;
        }
        /* 处理转义序列 */
        else
        {
            unsigned char sequence_length = 2;/*普通转义序列长度（\ + 字符 → 2字节）*/
            /* 合法性检查：转义符后无字符 → 非法 */
            if ((input_end - input_pointer) < 1)
            {
                goto fail;
            }

            /* 根据转义字符类型处理 */
            switch (input_pointer[1])
            {
                case 'b':/*退格符 \b → ASCII 0x08*/
                    *output_pointer++ = '\b';
                    break;
                case 'f':/*换页符 \f → ASCII 0x0C*/
                    *output_pointer++ = '\f';
                    break;
                case 'n':/*换行符 \n → ASCII 0x0A*/
                    *output_pointer++ = '\n';
                    break;
                case 'r':/*回车符 \r → ASCII 0x0D*/
                    *output_pointer++ = '\r';
                    break;
                case 't':/*制表符 \t → ASCII 0x09*/
                    *output_pointer++ = '\t';
                    break;
                case '\"':
                case '\\':
                case '/':
                    *output_pointer++ = input_pointer[1];
                    break;

                /* UTF-16 literal */
                case 'u':/*UTF-16转义序列 \uXXXX*/
                    /*转换为UTF-8，返回消耗的字节数（6/12）*/
                    sequence_length = utf16_literal_to_utf8(input_pointer, input_end, &output_pointer);
                    if (sequence_length == 0)
                    {
                        /* failed to convert UTF16-literal to UTF-8 */
                        goto fail;
                    }
                    break;

                default:/*未知转义字符 → 非法*/
                    goto fail;
            }
            input_pointer += sequence_length;/*跳过已处理的转义序列*/
        }
    }

    /* 输出字符串添加终止符 '\0' */
    *output_pointer = '\0';

    item->type = cJSON_String;/*标记节点类型为字符串*/
    item->valuestring = (char*)output;/*指向无转义的字符串缓冲区*/

    /* 更新解析缓冲区偏移量：跳过整个字符串（包括开头和结束双引号） */
    input_buffer->offset = (size_t) (input_end - input_buffer->content);
    input_buffer->offset++;

    return true;

fail:
    /* 失败清理：释放输出缓冲区 */
    if (output != NULL)
    {
        input_buffer->hooks.deallocate(output);
        output = NULL;
    }
    /* 更新解析偏移量到错误位置，方便后续定位 */
    if (input_pointer != NULL)
    {
        input_buffer->offset = (size_t)(input_pointer - input_buffer->content);
    }

    return false;
}

/* 将C字符串渲染为JSON兼容的转义字符串（生成器核心函数）
 * 作用：处理特殊字符转义（如"→\"、\n→\n），非ASCII字符转\uXXXX
 * 参数：
 *   input - 待转义的C字符串
 *   output_buffer - 打印缓冲区（存储转义后的JSON字符串）
 * 返回值：true=渲染成功；false=失败
 */
static cJSON_bool print_string_ptr(const unsigned char * const input, printbuffer * const output_buffer)
{
    const unsigned char *input_pointer = NULL;/*输入字符串遍历指针*/
    unsigned char *output = NULL;             /*输出缓冲区起始指针*/
    unsigned char *output_pointer = NULL;     /*输出缓冲区写入指针*/
    size_t output_length = 0;                 /*转义后的字符串长度*/
    /* numbers of additional characters needed for escaping */
    size_t escape_characters = 0;             /*转义需要的额外字符数（如\n→多1字节）*/

    /* 合法性检查：缓冲区为空 → 失败 */
    if (output_buffer == NULL)
    {
        return false;
    }

    /* 处理空字符串：直接输出"" */
    if (input == NULL)
    {
        output = ensure(output_buffer, sizeof("\"\""));/*确保缓冲区有2字节（""）*/
        if (output == NULL)
        {
            return false;
        }
        strcpy((char*)output, "\"\"");/*写入空字符串*/

        return true;
    }

    /* 第一步：统计需要转义的字符数，计算最终输出长度 */
    for (input_pointer = input; *input_pointer; input_pointer++)
    {
        switch (*input_pointer)
        {
            case '\"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                /* 单字符转义序列（\ + 字符 → 多1字节）*/
                escape_characters++;
                break;
            default:
                if (*input_pointer < 32)
                {
                    /* 控制字符（<32）：转\uXXXX → 多5字节（如\x01→\u0001） */
                    escape_characters += 5;
                }
                break;
        }
    }
    output_length = (size_t)(input_pointer - input) + escape_characters;/*输出总长度 = 原字符串长度 + 转义额外字符数*/

    /* 第二步：确保输出缓冲区有足够空间（+2为前后双引号 +1为终止符） */
    output = ensure(output_buffer, output_length + sizeof("\"\""));
    if (output == NULL)
    {
        return false;
    }

    /* 第三步：无转义字符 → 直接拷贝并添加双引号 */
    if (escape_characters == 0)
    {
        output[0] = '\"';
        memcpy(output + 1, input, output_length);
        output[output_length + 1] = '\"';
        output[output_length + 2] = '\0';

        return true;
    }

    /* 第四步：有转义字符 → 逐字符处理转义 */
    output[0] = '\"';/*开头双引号*/
    output_pointer = output + 1;/*指向字符串内容起始*/
    /* copy the string */
    for (input_pointer = input; *input_pointer != '\0'; (void)input_pointer++, output_pointer++)
    {
        /* 普通字符（可打印、非转义）：直接拷贝 */
        if ((*input_pointer > 31) && (*input_pointer != '\"') && (*input_pointer != '\\'))
        {
            /* normal character, copy */
            *output_pointer = *input_pointer;
        }
        else
        {
            /* 特殊字符：转义处理 */
            *output_pointer++ = '\\';
            switch (*input_pointer)
            {
                case '\\':
                    *output_pointer = '\\';
                    break;
                case '\"':
                    *output_pointer = '\"';
                    break;
                case '\b':
                    *output_pointer = 'b';
                    break;
                case '\f':
                    *output_pointer = 'f';
                    break;
                case '\n':
                    *output_pointer = 'n';
                    break;
                case '\r':
                    *output_pointer = 'r';
                    break;
                case '\t':
                    *output_pointer = 't';
                    break;
                default:
                    /* escape and print as unicode codepoint */
                    sprintf((char*)output_pointer, "u%04x", *input_pointer);
                    output_pointer += 4;
                    break;
            }
        }
    }
    output[output_length + 1] = '\"';
    output[output_length + 2] = '\0';

    return true;
}

/* 封装print_string_ptr，用于cJSON字符串节点的渲染
 * 参数：item - cJSON字符串节点；p - 打印缓冲区
 * 返回值：print_string_ptr的执行结果
 */
static cJSON_bool print_string(const cJSON * const item, printbuffer * const p)
{
    return print_string_ptr((unsigned char*)item->valuestring, p);
}

/* 预声明核心解析/生成函数原型（解决循环依赖）
 * parse_value：解析任意JSON值（字符串/数字/数组/对象/布尔/null）
 * print_value：生成任意JSON值的字符串
 * parse_array：解析JSON数组
 * print_array：生成JSON数组的字符串
 * parse_object：解析JSON对象
 * print_object：生成JSON对象的字符串
 */
static cJSON_bool parse_value(cJSON * const item, parse_buffer * const input_buffer);
static cJSON_bool print_value(const cJSON * const item, printbuffer * const output_buffer);
static cJSON_bool parse_array(cJSON * const item, parse_buffer * const input_buffer);
static cJSON_bool print_array(const cJSON * const item, printbuffer * const output_buffer);
static cJSON_bool parse_object(cJSON * const item, parse_buffer * const input_buffer);
static cJSON_bool print_object(const cJSON * const item, printbuffer * const output_buffer);

/* 辅助函数：跳过空白字符（空格、制表符、换行、回车等）
 * 作用：JSON语法忽略空白字符，解析前需跳过
 * 参数：buffer - 解析缓冲区
 * 返回值：跳过空白后的缓冲区指针（NULL=入参非法）
 */
static parse_buffer *buffer_skip_whitespace(parse_buffer * const buffer)
{
    /* 入参合法性检查 */
    if ((buffer == NULL) || (buffer->content == NULL))
    {
        return NULL;
    }

    /* 缓冲区已到末尾 → 直接返回 */
    if (cannot_access_at_index(buffer, 0))
    {
        return buffer;
    }

    /* 遍历并跳过所有空白字符（ASCII ≤32） */
    while (can_access_at_index(buffer, 0) && (buffer_at_offset(buffer)[0] <= 32))
    {
       buffer->offset++;
    }

    /* 边界处理：若偏移量等于总长度，回退1位（避免越界） */
    if (buffer->offset == buffer->length)
    {
        buffer->offset--;
    }

    return buffer;
}

/* 辅助函数：跳过UTF-8 BOM（字节顺序标记）
 * 背景：部分文件开头会加EF BB BF（UTF-8 BOM），JSON解析需忽略
 * 参数：buffer - 解析缓冲区（offset必须为0，即起始位置）
 * 返回值：跳过BOM后的缓冲区指针（NULL=入参非法/无BOM）
 */
static parse_buffer *skip_utf8_bom(parse_buffer * const buffer)
{
    /* 入参合法性检查：缓冲区空/非起始位置 → 返回NULL */
    if ((buffer == NULL) || (buffer->content == NULL) || (buffer->offset != 0))
    {
        return NULL;
    }

    /* 检查前3字节是否为EF BB BF（UTF-8 BOM） */
    if (can_access_at_index(buffer, 4) && (strncmp((const char*)buffer_at_offset(buffer), "\xEF\xBB\xBF", 3) == 0))
    {
        buffer->offset += 3;
    }

    return buffer;
}

/* JSON解析顶层接口（带选项）
 * 作用：将JSON字符串解析为cJSON树形结构
 * 参数：
 *   value - 待解析的JSON字符串
 *   return_parse_end - 输出参数，返回解析结束的位置（NULL=不返回）
 *   require_null_terminated - 是否要求JSON字符串以\0结尾（true=不允许末尾有垃圾字符）
 * 返回值：成功返回cJSON根节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithOpts(const char *value, const char **return_parse_end, cJSON_bool require_null_terminated)
{
    size_t buffer_length;

    /* 入参合法性检查：字符串为空 → 返回NULL */
    if (NULL == value)
    {
        return NULL;
    }

    /* 计算缓冲区长度：字符串长度 + 终止符（适应require_null_terminated） */
    buffer_length = strlen(value) + sizeof("");

    /* 调用带长度的解析接口（核心解析逻辑） */
    return cJSON_ParseWithLengthOpts(value, buffer_length, return_parse_end, require_null_terminated);
}

/* JSON解析核心入口（带长度和选项）
 * 参数：
 *   value - 待解析的JSON字符串
 *   buffer_length - 字符串长度（含终止符）
 *   return_parse_end - 输出参数，返回解析结束位置
 *   require_null_terminated - 是否要求严格以\0结尾
 * 返回值：成功返回cJSON根节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, cJSON_bool require_null_terminated)
{
    parse_buffer buffer = { 0, 0, 0, 0, { 0, 0, 0 } };
    cJSON *item = NULL;

    /* 重置全局错误信息（解析前清空上次错误） */
    global_error.json = NULL;
    global_error.position = 0;

    /* 入参合法性检查：字符串空/长度为0 → 失败 */
    if (value == NULL || 0 == buffer_length)
    {
        goto fail;
    }

    /* 初始化解析缓冲区 */
    buffer.content = (const unsigned char*)value;
    buffer.length = buffer_length;
    buffer.offset = 0;
    buffer.hooks = global_hooks;

    /* 创建根cJSON节点（解析的起点） */
    item = cJSON_New_Item(&global_hooks);
    if (item == NULL) /* 内存分配失败 */
    {
        goto fail;
    }

    /* 核心解析逻辑：
     * 1. 跳过UTF-8 BOM → 2. 跳过空白字符 → 3. 解析JSON值（递归解析）
     */
    if (!parse_value(item, buffer_skip_whitespace(skip_utf8_bom(&buffer))))
    {
        /* parse failure. ep is set. */
        goto fail;
    }

    /* 严格模式检查：要求JSON字符串以\0结尾，无末尾垃圾字符 */
    if (require_null_terminated)
    {
        buffer_skip_whitespace(&buffer);
        if ((buffer.offset >= buffer.length) || buffer_at_offset(&buffer)[0] != '\0')
        {
            goto fail;
        }
    }
    /* 输出解析结束位置（若需要） */
    if (return_parse_end)
    {
        *return_parse_end = (const char*)buffer_at_offset(&buffer);
    }

    /* 输出解析结束位置（若需要） */
    return item;

fail:
    /* 失败清理：释放已创建的cJSON节点 */
    if (item != NULL)
    {
        cJSON_Delete(item);
    }

    /* 记录错误位置，方便用户定位 */
    if (value != NULL)
    {
        error local_error;
        local_error.json = (const unsigned char*)value;
        local_error.position = 0;

        /* 定位错误位置：优先用解析偏移量，否则用缓冲区末尾 */
        if (buffer.offset < buffer.length)
        {
            local_error.position = buffer.offset;
        }
        else if (buffer.length > 0)
        {
            local_error.position = buffer.length - 1;
        }

        /* 输出解析结束位置（失败时指向错误位置） */
        if (return_parse_end != NULL)
        {
            *return_parse_end = (const char*)local_error.json + local_error.position;
        }

        /* 更新全局错误信息 */
        global_error = local_error;
    }

    return NULL;
}

/* cJSON_Parse 默认选项封装：无解析结束位置返回、不要求严格\0结尾
 * 作用：提供简化的JSON解析入口，适配大多数基础场景
 * 参数：value - 待解析的JSON字符串
 * 返回值：成功返回cJSON根节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *value)
{
    // 调用带选项的解析接口，return_parse_end=0，require_null_terminated=0
    return cJSON_ParseWithOpts(value, 0, 0);
}

/* 带长度的JSON解析接口（无选项）
 * 作用：支持解析非\0结尾的JSON字符串（通过指定长度）
 * 参数：
 *   value - 待解析的JSON字符串
 *   buffer_length - 字符串长度（不含终止符也可）
 * 返回值：成功返回cJSON根节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    return cJSON_ParseWithLengthOpts(value, buffer_length, 0, 0);
}

/* 最小值宏：返回a和b中的较小值
 * 作用：内存拷贝时防止越界（如缓冲区长度 < 需拷贝长度时，取较小值）
 */
#define cjson_min(a, b) (((a) < (b)) ? (a) : (b))

/* JSON生成器核心函数：将cJSON节点转为JSON字符串（内部接口）
 * 参数：
 *   item - 待生成的cJSON根节点
 *   format - 是否格式化输出（true=带缩进/空格，false=紧凑格式）
 *   hooks - 内存钩子（指定分配/释放函数）
 * 返回值：成功返回JSON字符串（需手动释放）；失败返回NULL
 */
static unsigned char *print(const cJSON * const item, cJSON_bool format, const internal_hooks * const hooks)
{
    static const size_t default_buffer_size = 256;
    printbuffer buffer[1];
    unsigned char *printed = NULL;

    /* 初始化打印缓冲区为0（避免野指针）*/
    memset(buffer, 0, sizeof(buffer));

    /* 第一步：创建初始打印缓冲区 */
    buffer->buffer = (unsigned char*) hooks->allocate(default_buffer_size);/*分配初始内存*/
    buffer->length = default_buffer_size;/*缓冲区总长度*/
    buffer->format = format;/*格式化标记*/
    buffer->hooks = *hooks;/*拷贝内存钩子*/
    if (buffer->buffer == NULL)
    {
        goto fail;
    }

    /* 第二步：核心生成逻辑：将cJSON节点转为字符串写入缓冲区 */
    if (!print_value(item, buffer))
    {
        goto fail;
    }
    update_offset(buffer);

    /* 第三步：优化缓冲区大小（缩容到实际使用长度） */
    if (hooks->reallocate != NULL)
    {
        /*有realloc函数：直接缩容（效率更高，无需拷贝）*/
        printed = (unsigned char*) hooks->reallocate(buffer->buffer, buffer->offset + 1);
        if (printed == NULL) {
            goto fail;
        }
        buffer->buffer = NULL;/*原缓冲区已被realloc接管，置空避免重复释放*/
    }
    else /*无realloc函数：手动分配新缓冲区 + 拷贝数据 + 释放原缓冲区 */
    {
        printed = (unsigned char*) hooks->allocate(buffer->offset + 1);
        if (printed == NULL)
        {
            goto fail;
        }
        /*拷贝数据（取缓冲区长度和实际使用长度的较小值，防止越界）*/
        memcpy(printed, buffer->buffer, cjson_min(buffer->length, buffer->offset + 1));
        printed[buffer->offset] = '\0'; /* just to be sure */

        /* 释放原缓冲区 */
        hooks->deallocate(buffer->buffer);
        buffer->buffer = NULL;
    }

    return printed;

fail:
    /* 失败清理：释放缓冲区和已分配的字符串 */
    if (buffer->buffer != NULL)
    {
        hooks->deallocate(buffer->buffer);
        buffer->buffer = NULL;
    }

    if (printed != NULL)
    {
        hooks->deallocate(printed);
        printed = NULL;
    }

    return NULL;
}

/* 格式化输出JSON字符串（公共接口）
 * 作用：将cJSON节点转为带缩进/空格的易读JSON字符串
 * 参数：item - cJSON根节点
 * 返回值：成功返回JSON字符串（需调用free释放）；失败返回NULL
 */
CJSON_PUBLIC(char *) cJSON_Print(const cJSON *item)
{
    /*调用内部print函数，format=true（格式化），使用全局内存钩子*/
    return (char*)print(item, true, &global_hooks);
}

/* 非格式化输出JSON字符串（公共接口）
 * 作用：将cJSON节点转为紧凑的JSON字符串（无多余空格/换行）
 * 参数：item - cJSON根节点
 * 返回值：成功返回JSON字符串（需调用free释放）；失败返回NULL
 */
CJSON_PUBLIC(char *) cJSON_PrintUnformatted(const cJSON *item)
{
    /*调用内部print函数，format=false（紧凑），使用全局内存钩子*/
    return (char*)print(item, false, &global_hooks);
}

/* 预分配缓冲区的JSON输出接口
 * 作用：用户指定初始缓冲区大小，减少动态扩容，提升性能
 * 参数：
 *   item - cJSON根节点
 *   prebuffer - 初始缓冲区大小（字节）
 *   fmt - 是否格式化输出
 * 返回值：成功返回JSON字符串（需调用free释放）；失败返回NULL
 */
CJSON_PUBLIC(char *) cJSON_PrintBuffered(const cJSON *item, int prebuffer, cJSON_bool fmt)
{
    printbuffer p = { 0, 0, 0, 0, 0, 0, { 0, 0, 0 } };

    if (prebuffer < 0)
    {
        return NULL;
    }

    p.buffer = (unsigned char*)global_hooks.allocate((size_t)prebuffer);
    if (!p.buffer)
    {
        return NULL;
    }

    p.length = (size_t)prebuffer;
    p.offset = 0;
    p.noalloc = false;
    p.format = fmt;
    p.hooks = global_hooks;

    if (!print_value(item, &p))
    {
        global_hooks.deallocate(p.buffer);
        p.buffer = NULL;
        return NULL;
    }

    return (char*)p.buffer;
}

/* 预分配缓冲区的JSON输出接口（不动态扩容）
 * 作用：使用用户提供的缓冲区，避免内存分配，适配嵌入式/内存受限场景
 * 参数：
 *   item - cJSON根节点
 *   buffer - 用户提供的输出缓冲区
 *   length - 缓冲区长度（字节）
 *   format - 是否格式化输出
 * 返回值：true=生成成功；false=失败（缓冲区不足/入参非法）
 */
CJSON_PUBLIC(cJSON_bool) cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const cJSON_bool format)
{
    printbuffer p = { 0, 0, 0, 0, 0, 0, { 0, 0, 0 } };

    if ((length < 0) || (buffer == NULL))
    {
        return false;
    }

    p.buffer = (unsigned char*)buffer;
    p.length = (size_t)length;
    p.offset = 0;
    p.noalloc = true;
    p.format = format;
    p.hooks = global_hooks;

    return print_value(item, &p);
}

/* 解析器核心：根据当前字符解析对应的JSON值类型（递归入口）
 * 作用：分发解析逻辑到具体类型（null/布尔/字符串/数字/数组/对象）
 * 参数：
 *   item - 待填充的cJSON节点
 *   input_buffer - 解析缓冲区
 * 返回值：true=解析成功；false=解析失败
 */
static cJSON_bool parse_value(cJSON * const item, parse_buffer * const input_buffer)
{
    if ((input_buffer == NULL) || (input_buffer->content == NULL))
    {
        return false; /* no input */
    }

    /* 按JSON语法依次匹配值类型 */
    /* null */
    if (can_read(input_buffer, 4) && (strncmp((const char*)buffer_at_offset(input_buffer), "null", 4) == 0))
    {
        item->type = cJSON_NULL;
        input_buffer->offset += 4;
        return true;
    }
    /* false */
    if (can_read(input_buffer, 5) && (strncmp((const char*)buffer_at_offset(input_buffer), "false", 5) == 0))
    {
        item->type = cJSON_False;
        input_buffer->offset += 5;
        return true;
    }
    /* true */
    if (can_read(input_buffer, 4) && (strncmp((const char*)buffer_at_offset(input_buffer), "true", 4) == 0))
    {
        item->type = cJSON_True;
        item->valueint = 1;
        input_buffer->offset += 4;
        return true;
    }
    /* string */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '\"'))
    {
        return parse_string(item, input_buffer);
    }
    /* number */
    if (can_access_at_index(input_buffer, 0) && ((buffer_at_offset(input_buffer)[0] == '-') || ((buffer_at_offset(input_buffer)[0] >= '0') && (buffer_at_offset(input_buffer)[0] <= '9'))))
    {
        return parse_number(item, input_buffer);
    }
    /* array */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '['))
    {
        return parse_array(item, input_buffer);
    }
    /* object */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '{'))
    {
        return parse_object(item, input_buffer);
    }

    return false;
}

/* 生成器核心：根据cJSON节点类型生成对应的JSON字符串（递归入口）
 * 作用：分发生成逻辑到具体类型（null/布尔/数字/字符串/数组/对象）
 * 参数：
 *   item - 待生成的cJSON节点
 *   output_buffer - 打印缓冲区
 * 返回值：true=生成成功；false=失败
 */
static cJSON_bool print_value(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output = NULL;

    if ((item == NULL) || (output_buffer == NULL))
    {
        return false;
    }

    /* 按节点类型分发生成逻辑（仅取低8位，屏蔽扩展标记） */
    switch ((item->type) & 0xFF)
    {
        case cJSON_NULL:
            output = ensure(output_buffer, 5);
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "null");
            return true;

        case cJSON_False:
            output = ensure(output_buffer, 6);
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "false");
            return true;

        case cJSON_True:
            output = ensure(output_buffer, 5);
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "true");
            return true;

        case cJSON_Number:
            return print_number(item, output_buffer);

        case cJSON_Raw:
        {
            size_t raw_length = 0;
            if (item->valuestring == NULL)
            {
                return false;
            }

            raw_length = strlen(item->valuestring) + sizeof("");
            output = ensure(output_buffer, raw_length);
            if (output == NULL)
            {
                return false;
            }
            memcpy(output, item->valuestring, raw_length);
            return true;
        }

        case cJSON_String:
            return print_string(item, output_buffer);

        case cJSON_Array:
            return print_array(item, output_buffer);

        case cJSON_Object:
            return print_object(item, output_buffer);

        default:
            return false;
    }
}

/* 解析JSON数组（核心函数）：将[元素1,元素2,...]解析为cJSON数组节点
 * 参数：
 *   item - 待填充的cJSON数组节点
 *   input_buffer - 解析缓冲区
 * 返回值：true=解析成功；false=失败
 */
static cJSON_bool parse_array(cJSON * const item, parse_buffer * const input_buffer)
{
    cJSON *head = NULL; /* 数组元素链表头节点 */
    cJSON *current_item = NULL;

    /*嵌套深度检查：超过限制 → 失败（防止栈溢出/恶意嵌套）*/
    if (input_buffer->depth >= CJSON_NESTING_LIMIT)
    {
        return false; /* to deeply nested */
    }
    input_buffer->depth++;

    /*合法性检查：当前字符不是[ → 不是数组*/
    if (buffer_at_offset(input_buffer)[0] != '[')
    {
        /* not an array */
        goto fail;
    }

    input_buffer->offset++;
    buffer_skip_whitespace(input_buffer);
    /*处理空数组（[后直接]）*/
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ']'))
    {
        /* empty array */
        goto success;
    }

    /* 边界检查：跳过空白后到缓冲区末尾 → 失败 */
    if (cannot_access_at_index(input_buffer, 0))
    {
        input_buffer->offset--;
        goto fail;
    }

    /* 回退偏移量（为后续循环做准备） */
    input_buffer->offset--;
    /* 循环解析数组元素（逗号分隔）*/
    do
    {
        /* 创建新元素节点 */
        cJSON *new_item = cJSON_New_Item(&(input_buffer->hooks));
        if (new_item == NULL)
        {
            goto fail; /* 内存分配失败 */
        }

        /* 将新节点加入链表 */
        if (head == NULL)
        {
            /* 第一个元素：初始化链表头 */
            current_item = head = new_item;
        }
        else
        {
            /* 后续元素：追加到链表末尾 */
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        /* 解析当前元素值 */
        input_buffer->offset++;
        buffer_skip_whitespace(input_buffer);
        if (!parse_value(current_item, input_buffer))
        {
            goto fail; /* 元素解析失败 */
        }
        buffer_skip_whitespace(input_buffer);
    }
    while (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ','));

    /*合法性检查：无结束符] → 失败*/
    if (cannot_access_at_index(input_buffer, 0) || buffer_at_offset(input_buffer)[0] != ']')
    {
        goto fail; /* expected end of array */
    }

success:
    input_buffer->depth--;

    /*链表闭环（头节点的prev指向尾节点）*/
    if (head != NULL) {
        head->prev = current_item;
    }

    /*填充数组节点*/
    item->type = cJSON_Array;
    item->child = head;

    input_buffer->offset++;

    return true;

fail:
    /*失败清理：释放已创建的元素节点*/
    if (head != NULL)
    {
        cJSON_Delete(head);
    }

    return false;
}

/* 生成JSON数组字符串（核心函数）：将cJSON数组节点转为[元素1,元素2,...]
 * 参数：
 *   item - cJSON数组节点
 *   output_buffer - 打印缓冲区
 * 返回值：true=生成成功；false=失败
 */
static cJSON_bool print_array(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;  /*缓冲区写入指针*/
    size_t length = 0;                     /*需分配的缓冲区长度*/
    cJSON *current_element = item->child;  /*当前遍历的数组元素*/

    /*入参合法性检查：缓冲区空 → 失败*/
    if (output_buffer == NULL)
    {
        return false;
    }

    /* 第一步：生成数组开头[ */
    output_pointer = ensure(output_buffer, 1);
    if (output_pointer == NULL)
    {
        return false;
    }

    *output_pointer = '[';
    output_buffer->offset++;
    output_buffer->depth++;

    /* 第二步：循环生成数组元素 */
    while (current_element != NULL)
    {
        /*生成当前元素的JSON字符串*/
        if (!print_value(current_element, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);
        /*有下一个元素：添加分隔符（, 或 , ）*/
        if (current_element->next)
        {
            /*格式化：, + 空格（2字节）；紧凑：,（1字节）*/
            length = (size_t) (output_buffer->format ? 2 : 1);
            output_pointer = ensure(output_buffer, length + 1);
            if (output_pointer == NULL)
            {
                return false;
            }
            *output_pointer++ = ',';
            if(output_buffer->format)
            {
                *output_pointer++ = ' ';
            }
            *output_pointer = '\0';
            output_buffer->offset += length;
        }
        current_element = current_element->next;/*遍历下一个元素*/
    }

    /* 第三步：生成数组结尾] */
    output_pointer = ensure(output_buffer, 2);
    if (output_pointer == NULL)
    {
        return false;
    }
    *output_pointer++ = ']';
    *output_pointer = '\0';
    output_buffer->depth--;

    return true;
}

/* 解析JSON对象（核心函数）：将{key1:value1,key2:value2,...}解析为cJSON对象节点
 * 参数：
 *   item - 待填充的cJSON对象节点
 *   input_buffer - 解析缓冲区
 * 返回值：true=解析成功；false=失败
 */
static cJSON_bool parse_object(cJSON * const item, parse_buffer * const input_buffer)
{
    cJSON *head = NULL; /* 对象键值对链表头节点 */
    cJSON *current_item = NULL;/*当前解析的键值对节点*/

    /*嵌套深度检查：超过限制 → 失败（防止栈溢出/恶意嵌套）*/
    if (input_buffer->depth >= CJSON_NESTING_LIMIT)
    {
        return false; /* to deeply nested */
    }
    input_buffer->depth++;

    /*合法性检查：缓冲区空 或 当前字符不是{ → 不是对象*/
    if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != '{'))
    {
        goto fail; /* not an object */
    }

    input_buffer->offset++;
    buffer_skip_whitespace(input_buffer);
    /*处理空对象（{后直接}）*/
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '}'))
    {
        goto success; /* empty object */
    }

    /* 边界检查：跳过空白后到缓冲区末尾 → 失败 */
    if (cannot_access_at_index(input_buffer, 0))
    {
        input_buffer->offset--;
        goto fail;
    }

    /* 回退偏移量（为后续循环做准备） */
    input_buffer->offset--;
    /* 循环解析对象的键值对（逗号分隔） */
    do
    {
        /* 创建新键值对节点 */
        cJSON *new_item = cJSON_New_Item(&(input_buffer->hooks));
        if (new_item == NULL)
        {
            goto fail; /* 内存分配失败 */
        }

        /* 将新节点加入链表 */
        if (head == NULL)
        {
            /* 第一个键值对：初始化链表头 */
            current_item = head = new_item;
        }
        else
        {
            /* 后续键值对：追加到链表末尾 */
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        /*合法性检查：逗号后无字符 → 失败*/
        if (cannot_access_at_index(input_buffer, 1))
        {
            goto fail; /* nothing comes after the comma */
        }

        /* 第一步：解析键名（JSON字符串） */
        input_buffer->offset++;
        buffer_skip_whitespace(input_buffer);
        if (!parse_string(current_item, input_buffer))
        {
            goto fail; /* failed to parse name */
        }
        buffer_skip_whitespace(input_buffer);

        /* 交换valuestring和string：
         * - parse_string会将键名存入valuestring
         * - cJSON对象节点的键名需存在string字段，值存在valuestring/valueint等
         */
        current_item->string = current_item->valuestring;
        current_item->valuestring = NULL;

        /*合法性检查：键名后无字符 或 不是冒号: → 非法对象*/
        if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != ':'))
        {
            goto fail; /* invalid object */
        }

        /* 第二步：解析键值（任意JSON值） */
        input_buffer->offset++;
        buffer_skip_whitespace(input_buffer);
        if (!parse_value(current_item, input_buffer))
        {
            goto fail; /* failed to parse value */
        }
        buffer_skip_whitespace(input_buffer);
    }
    while (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ','));/*有逗号则继续解析*/

    /*合法性检查：无结束符} → 失败*/
    if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != '}'))
    {
        goto fail; /* expected end of object */
    }

success:
    input_buffer->depth--;

    /*链表闭环（头节点的prev指向尾节点）*/
    if (head != NULL) {
        head->prev = current_item;
    }

    /*填充对象节点*/
    item->type = cJSON_Object;
    item->child = head;

    input_buffer->offset++;
    return true;

fail:
    if (head != NULL)
    {
        cJSON_Delete(head);
    }

    return false;
}

/* 生成JSON对象字符串（核心函数）：将cJSON对象节点转为{key1:value1,key2:value2,...}
 * 参数：
 *   item - cJSON对象节点
 *   output_buffer - 打印缓冲区
 * 返回值：true=生成成功；false=失败
 */
static cJSON_bool print_object(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;/*缓冲区写入指针*/
    size_t length = 0;                   /*需分配的缓冲区长度*/
    cJSON *current_item = item->child;   /*当前遍历的键值对*/

    /*入参合法性检查：缓冲区空 → 失败*/
    if (output_buffer == NULL)
    {
        return false;
    }

    /* 第一步：生成对象开头{
     * - 格式化：{ + \n（2字节）
     * - 紧凑：{（1字节）
     */
    length = (size_t) (output_buffer->format ? 2 : 1); /* fmt: {\n */
    output_pointer = ensure(output_buffer, length + 1);
    if (output_pointer == NULL)
    {
        return false;
    }

    *output_pointer++ = '{';
    output_buffer->depth++;
    if (output_buffer->format)
    {
        *output_pointer++ = '\n';
    }
    output_buffer->offset += length;

    /* 第二步：循环生成键值对 */
    while (current_item)
    {
        /*格式化：添加缩进（深度=当前嵌套深度的制表符）*/
        if (output_buffer->format)
        {
            size_t i;
            output_pointer = ensure(output_buffer, output_buffer->depth);
            if (output_pointer == NULL)
            {
                return false;
            }
            for (i = 0; i < output_buffer->depth; i++)
            {
                *output_pointer++ = '\t';
            }
            output_buffer->offset += output_buffer->depth;
        }

        /* 生成键名字符串（JSON格式，带双引号） */
        if (!print_string_ptr((unsigned char*)current_item->string, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);

        /*生成冒号分隔符：
         *- 格式化：: + \t（2字节）
         *- 紧凑：:（1字节） */
        length = (size_t) (output_buffer->format ? 2 : 1);
        output_pointer = ensure(output_buffer, length);
        if (output_pointer == NULL)
        {
            return false;
        }
        *output_pointer++ = ':';
        if (output_buffer->format)
        {
            *output_pointer++ = '\t';
        }
        output_buffer->offset += length;

        /* 生成键值（任意JSON值） */
        if (!print_value(current_item, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);

        /* 生成分隔符（逗号+换行 或 空）
         * length = 格式化换行(1) + 下一个元素则加逗号(1)
         */
        length = ((size_t)(output_buffer->format ? 1 : 0) + (size_t)(current_item->next ? 1 : 0));
        output_pointer = ensure(output_buffer, length + 1);
        if (output_pointer == NULL)
        {
            return false;
        }
        if (current_item->next)
        {
            *output_pointer++ = ',';/*非最后一个键值对，加逗号*/
        }

        if (output_buffer->format)
        {
            *output_pointer++ = '\n';/*格式化时换行*/
        }
        *output_pointer = '\0';
        output_buffer->offset += length;

        current_item = current_item->next;
    }

    /* 第三步：生成对象结尾} 
     * 需分配的长度：格式化=缩进长度+1（}）；紧凑=2（} + \0）
     */
    output_pointer = ensure(output_buffer, output_buffer->format ? (output_buffer->depth + 1) : 2);
    if (output_pointer == NULL)
    {
        return false;
    }
    /*格式化：添加缩进（深度-1个制表符）*/
    if (output_buffer->format)
    {
        size_t i;
        for (i = 0; i < (output_buffer->depth - 1); i++)
        {
            *output_pointer++ = '\t';
        }
    }
    *output_pointer++ = '}';
    *output_pointer = '\0';
    output_buffer->depth--;

    return true;
}

/* 获取JSON数组的元素个数（公共接口）
 * 参数：array - cJSON数组节点
 * 返回值：成功返回元素个数；失败/空数组返回0（注意：数组过大时可能溢出int）
 */
CJSON_PUBLIC(int) cJSON_GetArraySize(const cJSON *array)
{
    cJSON *child = NULL;
    size_t size = 0;

    /*入参合法性检查：数组节点空 → 返回0*/
    if (array == NULL)
    {
        return 0;
    }

    child = array->child;

    /*遍历链表，统计元素个数*/
    while(child != NULL)
    {
        size++;
        child = child->next;
    }

    /* 注意：此处有溢出风险（size_t转int），但为了兼容旧API无法修改 */

    return (int)size;
}

/* 内部接口：按索引获取数组元素（size_t版，避免int溢出）
 * 参数：
 *   array - cJSON数组节点
 *   index - 元素索引（从0开始）
 * 返回值：成功返回对应元素节点；失败返回NULL
 */
static cJSON* get_array_item(const cJSON *array, size_t index)
{
    cJSON *current_child = NULL;

    if (array == NULL)
    {
        return NULL;
    }

    current_child = array->child;
    while ((current_child != NULL) && (index > 0))
    {
        index--;
        current_child = current_child->next;
    }

    return current_child;
}

/* 公共接口：按索引获取数组元素（int版）
 * 参数：
 *   array - cJSON数组节点
 *   index - 元素索引（从0开始，负数返回NULL）
 * 返回值：成功返回对应元素节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_GetArrayItem(const cJSON *array, int index)
{
    if (index < 0)
    {
        return NULL;
    }

    return get_array_item(array, (size_t)index);
}

/* 内部接口：按键名获取对象的键值对节点
 * 参数：
 *   object - cJSON对象节点
 *   name - 要查找的键名
 *   case_sensitive - 是否大小写敏感（true=敏感，false=不敏感）
 * 返回值：成功返回对应节点；失败返回NULL
 */
static cJSON *get_object_item(const cJSON * const object, const char * const name, const cJSON_bool case_sensitive)
{
    cJSON *current_element = NULL;

    if ((object == NULL) || (name == NULL))
    {
        return NULL;
    }

    current_element = object->child;
    if (case_sensitive)
    {
        /*大小写敏感：用strcmp比较*/
        while ((current_element != NULL) && (current_element->string != NULL) && (strcmp(name, current_element->string) != 0))
        {
            current_element = current_element->next;
        }
    }
    else
    {
        /*大小写不敏感：用自定义的不敏感比较函数*/
        while ((current_element != NULL) && (case_insensitive_strcmp((const unsigned char*)name, (const unsigned char*)(current_element->string)) != 0))
        {
            current_element = current_element->next;
        }
    }

    /*检查：找到的节点必须有键名（避免空节点）*/
    if ((current_element == NULL) || (current_element->string == NULL)) {
        return NULL;
    }

    return current_element;
}

/* 公共接口：按键名获取对象节点（大小写不敏感）
 * 作用：兼容大多数场景的对象查询，忽略键名大小写
 * 参数：
 *   object - cJSON对象节点
 *   string - 要查找的键名
 * 返回值：成功返回对应节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_GetObjectItem(const cJSON * const object, const char * const string)
{
    return get_object_item(object, string, false);
}

/* 公共接口：按键名获取对象节点（大小写敏感）
 * 作用：精准匹配键名大小写，适用于严格场景
 * 参数：
 *   object - cJSON对象节点
 *   string - 要查找的键名
 * 返回值：成功返回对应节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string)
{
    return get_object_item(object, string, true);
}

/* 公共接口：检查对象是否包含指定键名（大小写不敏感）
 * 作用：快速判断键是否存在，无需获取完整节点
 * 参数：
 *   object - cJSON对象节点
 *   string - 要检查的键名
 * 返回值：1=存在；0=不存在
 */
CJSON_PUBLIC(cJSON_bool) cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItem(object, string) ? 1 : 0;
}

/* 工具函数：将节点追加到链表末尾（维护双向链表关系）
 * 作用：辅助数组/对象添加节点，设置prev/next指针
 * 参数：
 *   prev - 链表尾节点
 *   item - 待追加的新节点
 */
static void suffix_object(cJSON *prev, cJSON *item)
{
    prev->next = item;
    item->prev = prev;
}

/* 工具函数：创建节点的引用（浅拷贝）
 * 作用：避免深拷贝，仅引用原节点数据，节省内存
 * 参数：
 *   item - 被引用的原节点
 *   hooks - 内存钩子
 * 返回值：成功返回引用节点；失败返回NULL
 */
static cJSON *create_reference(const cJSON *item, const internal_hooks * const hooks)
{
    cJSON *reference = NULL;
    /*入参合法性检查：原节点空 → 返回NULL*/
    if (item == NULL)
    {
        return NULL;
    }

    /*创建新节点（引用节点）*/
    reference = cJSON_New_Item(hooks);
    if (reference == NULL)
    {
        return NULL;
    }

    /*浅拷贝原节点所有字段（共享数据，不拷贝字符串）*/
    memcpy(reference, item, sizeof(cJSON));
    reference->string = NULL;                /*引用节点不持有键名字符串*/
    reference->type |= cJSON_IsReference;    /*标记为引用类型*/
    reference->next = reference->prev = NULL;/*重置链表指针*/
    return reference;
}

/* 内部接口：将节点添加到数组末尾
 * 作用：维护数组的双向链表结构，支持空数组/非空数组
 * 参数：
 *   array - cJSON数组节点
 *   item - 待添加的新节点
 * 返回值：true=成功；false=失败
 */
static cJSON_bool add_item_to_array(cJSON *array, cJSON *item)
{
    cJSON *child = NULL;

    /*入参合法性检查：节点空/数组空/自引用 → 失败*/
    if ((item == NULL) || (array == NULL) || (array == item))
    {
        return false;
    }

    child = array->child;/*指向数组第一个元素*/
    
    /* 优化：利用child->prev快速找到数组最后一个元素（闭环链表） */
    if (child == NULL)
    {
        /* list is empty, start new one */
        array->child = item;/*数组child指向新节点*/
        item->prev = item;  /*新节点prev自指向（标记尾节点）*/
        item->next = NULL;  /*新节点next为空*/
    }
    else
    {
        /* 非空数组：追加到末尾 */
        if (child->prev)
        {
            suffix_object(child->prev, item);/*尾节点追加新节点*/
            array->child->prev = item;       /*更新头节点prev指向新尾节点*/
        }
    }

    return true;
}

/* 公共接口：将节点添加到数组末尾
 * 参数：
 *   array - cJSON数组节点
 *   item - 待添加的新节点
 * 返回值：true=成功；false=失败
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    return add_item_to_array(array, item);
}

/* 编译器兼容：临时关闭GCC/Clang的"const转换"警告
 * 作用：cast_away_const函数需要将const指针转为非const，避免编译器告警
 */
#if defined(__clang__) || (defined(__GNUC__)  && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
    #pragma GCC diagnostic push
#endif
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
/* 工具函数：移除指针的const属性（仅用于合法场景）
 * 参数：string - const指针
 * 返回值：非const指针
 */
static void* cast_away_const(const void* string)
{
    return (void*)string;
}
#if defined(__clang__) || (defined(__GNUC__)  && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
    #pragma GCC diagnostic pop
#endif


/* 内部接口：将节点添加到对象（设置键名）
 * 作用：处理键名的内存管理（拷贝/引用），复用数组添加逻辑
 * 参数：
 *   object - cJSON对象节点
 *   string - 键名
 *   item - 待添加的节点
 *   hooks - 内存钩子
 *   constant_key - 是否使用常量键名（true=不拷贝，false=拷贝）
 * 返回值：true=成功；false=失败
 */
static cJSON_bool add_item_to_object(cJSON * const object, const char * const string, cJSON * const item, const internal_hooks * const hooks, const cJSON_bool constant_key)
{
    char *new_key = NULL;
    int new_type = cJSON_Invalid;

    /*入参合法性检查：对象/键名/节点空 或 自引用 → 失败*/
    if ((object == NULL) || (string == NULL) || (item == NULL) || (object == item))
    {
        return false;
    }

    if (constant_key)
    {
        /*常量键名：直接引用，不拷贝（节省内存）*/
        new_key = (char*)cast_away_const(string);
        new_type = item->type | cJSON_StringIsConst;
    }
    else
    {
        /*非常量键名：拷贝键名字符串（避免原字符串释放导致崩溃）*/
        new_key = (char*)cJSON_strdup((const unsigned char*)string, hooks);
        if (new_key == NULL)
        {
            return false;
        }

        new_type = item->type & ~cJSON_StringIsConst;
    }

    /*释放节点原有键名（避免内存泄漏）*/
    if (!(item->type & cJSON_StringIsConst) && (item->string != NULL))
    {
        hooks->deallocate(item->string);
    }

    /*设置新键名和类型*/
    item->string = new_key;
    item->type = new_type;

    /*复用数组添加逻辑（对象和数组底层都是双向链表）*/
    return add_item_to_array(object, item);
}

/* 公共接口：将节点添加到对象（拷贝键名）
 * 参数：
 *   object - cJSON对象节点
 *   string - 键名（会拷贝）
 *   item - 待添加的节点
 * 返回值：true=成功；false=失败
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    return add_item_to_object(object, string, item, &global_hooks, false);
}

/* 公共接口：将节点添加到对象（使用常量键名）
 * 作用：键名不拷贝，直接引用，适用于常量字符串（如字面量）
 * 参数：
 *   object - cJSON对象节点
 *   string - 常量键名（不拷贝）
 *   item - 待添加的节点
 * 返回值：true=成功；false=失败
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item)
{
    return add_item_to_object(object, string, item, &global_hooks, true);
}

/* 公共接口：添加节点引用到数组（浅拷贝）
 * 作用：避免深拷贝，仅引用原节点，节省内存
 * 参数：
 *   array - cJSON数组节点
 *   item - 被引用的原节点
 * 返回值：true=成功；false=失败
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item)
{
    if (array == NULL)
    {
        return false;
    }

    return add_item_to_array(array, create_reference(item, &global_hooks));
}

/* 公共接口：添加节点引用到对象（浅拷贝）
 * 参数：
 *   object - cJSON对象节点
 *   string - 键名（会拷贝）
 *   item - 被引用的原节点
 * 返回值：true=成功；false=失败
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item)
{
    if ((object == NULL) || (string == NULL))
    {
        return false;
    }

    return add_item_to_object(object, string, create_reference(item, &global_hooks), &global_hooks, false);
}

/* 快捷接口：向对象添加null节点
 * 参数：
 *   object - cJSON对象节点
 *   name - 键名
 * 返回值：成功返回新节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON*) cJSON_AddNullToObject(cJSON * const object, const char * const name)
{
    cJSON *null = cJSON_CreateNull();
    if (add_item_to_object(object, name, null, &global_hooks, false))
    {
        return null;
    }

    cJSON_Delete(null);
    return NULL;
}

/* 快捷接口：向对象添加true节点
 * 参数/返回值：同AddNullToObject
 */
CJSON_PUBLIC(cJSON*) cJSON_AddTrueToObject(cJSON * const object, const char * const name)
{
    cJSON *true_item = cJSON_CreateTrue();
    if (add_item_to_object(object, name, true_item, &global_hooks, false))
    {
        return true_item;
    }

    cJSON_Delete(true_item);
    return NULL;
}

/* 快捷接口：向对象添加false节点
 * 参数/返回值：同AddNullToObject
 */
CJSON_PUBLIC(cJSON*) cJSON_AddFalseToObject(cJSON * const object, const char * const name)
{
    cJSON *false_item = cJSON_CreateFalse();
    if (add_item_to_object(object, name, false_item, &global_hooks, false))
    {
        return false_item;
    }

    cJSON_Delete(false_item);
    return NULL;
}

/* 快捷接口：向对象添加布尔节点
 * 参数：
 *   object - cJSON对象节点
 *   name - 键名
 *   boolean - 布尔值（1=true，0=false）
 * 返回值：同AddNullToObject
 */
CJSON_PUBLIC(cJSON*) cJSON_AddBoolToObject(cJSON * const object, const char * const name, const cJSON_bool boolean)
{
    cJSON *bool_item = cJSON_CreateBool(boolean);
    if (add_item_to_object(object, name, bool_item, &global_hooks, false))
    {
        return bool_item;
    }

    cJSON_Delete(bool_item);
    return NULL;
}

/* 快捷接口：向对象添加数字节点
 * 参数：
 *   object - cJSON对象节点
 *   name - 键名
 *   number - 数字值（double）
 * 返回值：同AddNullToObject
 */
CJSON_PUBLIC(cJSON*) cJSON_AddNumberToObject(cJSON * const object, const char * const name, const double number)
{
    cJSON *number_item = cJSON_CreateNumber(number);
    if (add_item_to_object(object, name, number_item, &global_hooks, false))
    {
        return number_item;
    }

    cJSON_Delete(number_item);
    return NULL;
}

/* 快捷接口：向对象添加字符串节点
 * 参数：
 *   object - cJSON对象节点
 *   name - 键名
 *   string - 字符串值
 * 返回值：同AddNullToObject
 */
CJSON_PUBLIC(cJSON*) cJSON_AddStringToObject(cJSON * const object, const char * const name, const char * const string)
{
    cJSON *string_item = cJSON_CreateString(string);
    if (add_item_to_object(object, name, string_item, &global_hooks, false))
    {
        return string_item;
    }

    cJSON_Delete(string_item);
    return NULL;
}

/* 快捷接口：向对象添加原始JSON节点（不转义）
 * 参数：
 *   object - cJSON对象节点
 *   name - 键名
 *   raw - 原始JSON字符串
 * 返回值：同AddNullToObject
 */
CJSON_PUBLIC(cJSON*) cJSON_AddRawToObject(cJSON * const object, const char * const name, const char * const raw)
{
    cJSON *raw_item = cJSON_CreateRaw(raw);
    if (add_item_to_object(object, name, raw_item, &global_hooks, false))
    {
        return raw_item;
    }

    cJSON_Delete(raw_item);
    return NULL;
}

/* 快捷接口：向对象添加子对象节点
 * 参数：
 *   object - 父对象节点
 *   name - 键名
 * 返回值：成功返回子对象节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON*) cJSON_AddObjectToObject(cJSON * const object, const char * const name)
{
    cJSON *object_item = cJSON_CreateObject();
    if (add_item_to_object(object, name, object_item, &global_hooks, false))
    {
        return object_item;
    }

    cJSON_Delete(object_item);
    return NULL;
}

/* 快捷接口：向对象添加子数组节点
 * 参数/返回值：同AddObjectToObject
 */
CJSON_PUBLIC(cJSON*) cJSON_AddArrayToObject(cJSON * const object, const char * const name)
{
    cJSON *array = cJSON_CreateArray();
    if (add_item_to_object(object, name, array, &global_hooks, false))
    {
        return array;
    }

    cJSON_Delete(array);
    return NULL;
}

/* 公共接口：从父节点中分离指定节点（保留节点，仅移除链表关系）
 * 作用：不删除节点，仅断开链表，可复用该节点
 * 参数：
 *   parent - 父节点（数组/对象）
 *   item - 待分离的节点
 * 返回值：成功返回分离的节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemViaPointer(cJSON *parent, cJSON * const item)
{
    if ((parent == NULL) || (item == NULL) || (item != parent->child && item->prev == NULL))
    {
        return NULL;
    }

    if (item != parent->child)
    {
        /* 非第一个节点：前驱节点的next指向后继节点 */
        item->prev->next = item->next;
    }
    if (item->next != NULL)
    {
        /* 非最后一个节点：后继节点的prev指向前驱节点 */
        item->next->prev = item->prev;
    }

    if (item == parent->child)
    {
        /* 第一个节点：父节点child指向后继节点 */
        parent->child = item->next;
    }
    else if (item->next == NULL)
    {
        /* 最后一个节点：更新头节点prev指向新尾节点 */
        parent->child->prev = item->prev;
    }

    /* 重置分离节点的链表指针（避免野指针） */
    item->prev = NULL;
    item->next = NULL;

    return item;
}

/* 公共接口：从数组中分离指定索引的节点
 * 参数：
 *   array - cJSON数组节点
 *   which - 元素索引（从0开始）
 * 返回值：成功返回分离的节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromArray(cJSON *array, int which)
{
    if (which < 0)
    {
        return NULL;
    }

    return cJSON_DetachItemViaPointer(array, get_array_item(array, (size_t)which));
}

/* 公共接口：从数组中删除指定索引的节点（分离+释放）
 * 参数：
 *   array - cJSON数组节点
 *   which - 元素索引
 */
CJSON_PUBLIC(void) cJSON_DeleteItemFromArray(cJSON *array, int which)
{
    cJSON_Delete(cJSON_DetachItemFromArray(array, which));
}

/* 公共接口：从对象中分离指定键名的节点（大小写不敏感）
 * 参数：
 *   object - cJSON对象节点
 *   string - 键名
 * 返回值：成功返回分离的节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObject(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItem(object, string);

    return cJSON_DetachItemViaPointer(object, to_detach);
}

/* 公共接口：从对象中分离指定键名的节点（大小写敏感）
 * 参数/返回值：同DetachItemFromObject
 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItemCaseSensitive(object, string);

    return cJSON_DetachItemViaPointer(object, to_detach);
}

/* 公共接口：从对象中删除指定键名的节点（大小写不敏感）
 * 参数：
 *   object - cJSON对象节点
 *   string - 键名
 */
CJSON_PUBLIC(void) cJSON_DeleteItemFromObject(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObject(object, string));
}

/* 公共接口：从对象中删除指定键名的节点（大小写敏感）
 * 参数：同DeleteItemFromObject
 */
CJSON_PUBLIC(void) cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObjectCaseSensitive(object, string));
}

/* 公共接口：在数组指定索引位置插入节点
 * 作用：支持中间插入，而非仅追加
 * 参数：
 *   array - cJSON数组节点
 *   which - 插入位置（0=开头，n=第n个元素后）
 *   newitem - 待插入的新节点
 * 返回值：true=成功；false=失败
 */
CJSON_PUBLIC(cJSON_bool) cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem)
{
    cJSON *after_inserted = NULL;

    if (which < 0 || newitem == NULL)
    {
        return false;
    }

    /*找到插入位置的后一个节点*/
    after_inserted = get_array_item(array, (size_t)which);
    if (after_inserted == NULL)
    {
        return add_item_to_array(array, newitem);/*插入位置超出数组长度 → 追加到末尾*/
    }

    /*合法性检查：节点链表损坏 → 失败*/
    if (after_inserted != array->child && after_inserted->prev == NULL) {
        /* return false if after_inserted is a corrupted array item */
        return false;
    }

    /*调整链表指针：插入新节点到after_inserted之前*/
    newitem->next = after_inserted;
    newitem->prev = after_inserted->prev;
    after_inserted->prev = newitem;
    if (after_inserted == array->child)
    {
        array->child = newitem;/*插入到开头：更新数组child指向新节点*/
    }
    else
    {
        newitem->prev->next = newitem;/*插入到中间：前驱节点的next指向新节点*/
    }
    return true;
}

/* 公共接口：替换父节点中的指定节点
 * 作用：替换节点内容，维护链表关系
 * 参数：
 *   parent - 父节点（数组/对象）
 *   item - 被替换的旧节点
 *   replacement - 新节点
 * 返回值：true=成功；false=失败
 */
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemViaPointer(cJSON * const parent, cJSON * const item, cJSON * replacement)
{
    if ((parent == NULL) || (parent->child == NULL) || (replacement == NULL) || (item == NULL))
    {
        return false;
    }

    if (replacement == item)
    {
        return true;/*新旧节点相同，无需替换*/
    }

    /*继承旧节点的链表关系*/
    replacement->next = item->next;
    replacement->prev = item->prev;

    /*更新后继节点的prev*/
    if (replacement->next != NULL)
    {
        replacement->next->prev = replacement;
    }
    /*替换第一个节点：更新父节点child*/
    if (parent->child == item)
    {
        if (parent->child->prev == parent->child)
        {
            replacement->prev = replacement;
        }
        parent->child = replacement;
    }
    else
    {   /*
         * To find the last item in array quickly, we use prev in array.
         * We can't modify the last item's next pointer where this item was the parent's child
         */
        if (replacement->prev != NULL)
        {
            replacement->prev->next = replacement;/*更新前驱节点的next*/
        }
        if (replacement->next == NULL)
        {
            parent->child->prev = replacement;/*更新尾节点标记*/
        }
    }

    /*重置旧节点指针并释放*/
    item->next = NULL;
    item->prev = NULL;
    cJSON_Delete(item);

    return true;
}

/* 公共接口：替换数组中指定索引的节点
 * 参数：
 *   array - cJSON数组节点
 *   which - 元素索引
 *   newitem - 新节点
 * 返回值：true=成功；false=失败
 */
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem)
{
    if (which < 0)
    {
        return false;
    }

    return cJSON_ReplaceItemViaPointer(array, get_array_item(array, (size_t)which), newitem);
}

/* 内部接口：替换对象中指定键名的节点
 * 参数：
 *   object - cJSON对象节点
 *   string - 键名
 *   replacement - 新节点
 *   case_sensitive - 是否大小写敏感
 * 返回值：true=成功；false=失败
 */
static cJSON_bool replace_item_in_object(cJSON *object, const char *string, cJSON *replacement, cJSON_bool case_sensitive)
{
    if ((replacement == NULL) || (string == NULL))
    {
        return false;
    }

    /* 设置新节点的键名（替换原有键名） */
    if (!(replacement->type & cJSON_StringIsConst) && (replacement->string != NULL))
    {
        cJSON_free(replacement->string);
    }
    /*拷贝新键名*/
    replacement->string = (char*)cJSON_strdup((const unsigned char*)string, &global_hooks);
    if (replacement->string == NULL)
    {
        return false;
    }

    replacement->type &= ~cJSON_StringIsConst;/*清除常量标记*/

    /*找到旧节点并替换*/
    return cJSON_ReplaceItemViaPointer(object, get_object_item(object, string, case_sensitive), replacement);
}

/* 公共接口：替换对象中指定键名的节点（大小写不敏感）
 * 参数：
 *   object - cJSON对象节点
 *   string - 键名
 *   newitem - 新节点
 * 返回值：true=成功；false=失败
 */
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, false);
}

/* 公共接口：替换对象中指定键名的节点（大小写敏感）
 * 参数/返回值：同ReplaceItemInObject
 */
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, true);
}

/* ===================== 基础类型节点创建接口 ===================== */

/* 公共接口：创建null类型节点
 * 返回值：成功返回null节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateNull(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_NULL;
    }

    return item;
}

/* 公共接口：创建true类型节点
 * 返回值：成功返回true节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateTrue(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_True;
    }

    return item;
}

/* 公共接口：创建false类型节点
 * 返回值：成功返回false节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateFalse(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_False;
    }

    return item;
}

/* 公共接口：创建布尔类型节点（通用）
 * 参数：boolean - 布尔值（1=true，0=false）
 * 返回值：成功返回布尔节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateBool(cJSON_bool boolean)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = boolean ? cJSON_True : cJSON_False;
    }

    return item;
}

/* 公共接口：创建数字类型节点
 * 参数：num - 数字值（double）
 * 返回值：成功返回数字节点；失败返回NULL
 * 关键设计：同步设置valuedouble（精确值）和valueint（整数近似值，兼容旧逻辑）
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_Number;
        item->valuedouble = num;

        /* use saturation in case of overflow */
        if (num >= INT_MAX)
        {
            item->valueint = INT_MAX;
        }
        else if (num <= (double)INT_MIN)
        {
            item->valueint = INT_MIN;
        }
        else
        {
            item->valueint = (int)num;
        }
    }

    return item;
}

/* 公共接口：创建字符串类型节点（拷贝字符串）
 * 参数：string - 字符串值（会拷贝，原字符串释放不影响）
 * 返回值：成功返回字符串节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_String;
        item->valuestring = (char*)cJSON_strdup((const unsigned char*)string, &global_hooks);
        if(!item->valuestring)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}

/* 公共接口：创建字符串类型节点（引用字符串，浅拷贝）
 * 作用：不拷贝字符串，直接引用，节省内存（适用于常量字符串）
 * 参数：string - 常量字符串（如字面量，生命周期长）
 * 返回值：成功返回字符串节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateStringReference(const char *string)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL)
    {
        item->type = cJSON_String | cJSON_IsReference;
        item->valuestring = (char*)cast_away_const(string);
    }

    return item;
}

/* 公共接口：创建对象类型节点（引用子节点，浅拷贝）
 * 参数：child - 被引用的子节点链表
 * 返回值：成功返回对象节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateObjectReference(const cJSON *child)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL) {
        item->type = cJSON_Object | cJSON_IsReference;
        item->child = (cJSON*)cast_away_const(child);
    }

    return item;
}

/* 公共接口：创建数组类型节点（引用子节点，浅拷贝）
 * 参数：child - 被引用的子节点链表
 * 返回值：成功返回数组节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateArrayReference(const cJSON *child) {
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL) {
        item->type = cJSON_Array | cJSON_IsReference;
        item->child = (cJSON*)cast_away_const(child);
    }

    return item;
}

/* 公共接口：创建原始JSON类型节点（不转义，直接拷贝）
 * 作用：存储无需转义的原始JSON片段（如"{\"key\":123}"）
 * 参数：raw - 原始JSON字符串
 * 返回值：成功返回原始节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateRaw(const char *raw)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_Raw;
        item->valuestring = (char*)cJSON_strdup((const unsigned char*)raw, &global_hooks);
        if(!item->valuestring)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}

/* 公共接口：创建空数组节点
 * 返回值：成功返回数组节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateArray(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type=cJSON_Array;
    }

    return item;
}

/* 公共接口：创建空对象节点
 * 返回值：成功返回对象节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateObject(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item)
    {
        item->type = cJSON_Object;
    }

    return item;
}

/* ===================== 批量数组创建接口 ===================== */

/* 公共接口：批量创建int数组节点
 * 参数：
 *   numbers - int数组（源数据）
 *   count - 数组长度
 * 返回值：成功返回包含所有int的数组节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateIntArray(const int *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if (!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

/* 公共接口：批量创建float数组节点
 * 参数：
 *   numbers - float数组（源数据）
 *   count - 数组长度
 * 返回值：成功返回包含所有float的数组节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateFloatArray(const float *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber((double)numbers[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

/* 公共接口：批量创建double数组节点
 * 参数：
 *   numbers - double数组（源数据）
 *   count - 数组长度
 * 返回值：成功返回包含所有double的数组节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateDoubleArray(const double *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

/* 公共接口：批量创建字符串数组节点
 * 参数：
 *   strings - 字符串数组（源数据）
 *   count - 数组长度
 * 返回值：成功返回包含所有字符串的数组节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateStringArray(const char *const *strings, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (strings == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for (i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateString(strings[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p,n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

/* ===================== 节点深拷贝接口 ===================== */

/*前向声明：递归深拷贝函数*/
cJSON * cJSON_Duplicate_rec(const cJSON *item, size_t depth, cJSON_bool recurse);

/* 公共接口：深拷贝cJSON节点（对外封装）
 * 参数：
 *   item - 被拷贝的源节点
 *   recurse - 是否递归拷贝子节点（true=深拷贝，false=浅拷贝）
 * 返回值：成功返回拷贝后的新节点；失败返回NULL
 */
CJSON_PUBLIC(cJSON *) cJSON_Duplicate(const cJSON *item, cJSON_bool recurse)
{
    /*调用递归拷贝函数，初始深度=0*/
    return cJSON_Duplicate_rec(item, 0, recurse );
}

/* 内部接口：递归深拷贝cJSON节点（核心实现）
 * 参数：
 *   item - 源节点
 *   depth - 当前递归深度（防止循环引用）
 *   recurse - 是否递归
 * 返回值：成功返回新节点；失败返回NULL
 */
cJSON * cJSON_Duplicate_rec(const cJSON *item, size_t depth, cJSON_bool recurse)
{
    cJSON *newitem = NULL;/*拷贝后的新节点*/
    cJSON *child = NULL;/*源节点的子节点*/
    cJSON *next = NULL;/*新节点的子节点链表指针*/
    cJSON *newchild = NULL;/*拷贝后的子节点*/

    /* 入参合法性检查：源节点空 → 失败 */
    if (!item)
    {
        goto fail;
    }
    /* 创建新节点（分配内存） */
    newitem = cJSON_New_Item(&global_hooks);
    if (!newitem)
    {
        goto fail;
    }
    /* 第一步：拷贝基础字段 */
    newitem->type = item->type & (~cJSON_IsReference);/*清除引用标记*/
    newitem->valueint = item->valueint;/*拷贝int值*/
    newitem->valuedouble = item->valuedouble;/*拷贝double值*/
    if (item->valuestring)
    {
        newitem->valuestring = (char*)cJSON_strdup((unsigned char*)item->valuestring, &global_hooks);
        /*拷贝失败 → 跳转失败逻辑*/
        if (!newitem->valuestring)
        {
            goto fail;
        }
    }
    /*拷贝string（对象键名）：常量键名直接引用，否则拷贝*/
    if (item->string)
    {
        newitem->string = (item->type&cJSON_StringIsConst) ? item->string : (char*)cJSON_strdup((unsigned char*)item->string, &global_hooks);
        if (!newitem->string)
        {
            goto fail;
        }
    }
    /* 第二步：非递归模式 → 直接返回（浅拷贝） */
    if (!recurse)
    {
        return newitem;
    }
    /* 第三步：递归拷贝子节点（数组/对象） */
    child = item->child;
    while (child != NULL)
    {
        /*循环引用保护：超过深度限制 → 失败*/
        if(depth >= CJSON_CIRCULAR_LIMIT) {
            goto fail;
        }
        newchild = cJSON_Duplicate_rec(child, depth + 1, true); /* 递归拷贝子节点（深度+1） */
        if (!newchild)
        {
            goto fail;
        }
        if (next != NULL)
        {
            /*  非第一个子节点：追加到链表末尾 */
            next->next = newchild;
            newchild->prev = next;
            next = newchild;
        }
        else
        {
            /* 第一个子节点：新节点child指向该子节点 */
            newitem->child = newchild;
            next = newchild;
        }
        child = child->next;
    }
    /*闭环链表：头节点prev指向尾节点*/
    if (newitem && newitem->child)
    {
        newitem->child->prev = newchild;
    }

    return newitem;

fail:
    if (newitem != NULL)
    {
        cJSON_Delete(newitem);
    }

    return NULL;
}

/* 工具函数：跳过单行注释（// 开头）
 * 作用：解析JSON时忽略单行注释，提升兼容性
 * 参数：input - 指向JSON字符串的指针（会修改指针位置）
 */
static void skip_oneline_comment(char **input)
{
    *input += static_strlen("//");

    for (; (*input)[0] != '\0'; ++(*input))
    {
        if ((*input)[0] == '\n') {
            *input += static_strlen("\n");
            return;
        }
    }
}

/* 工具函数：跳过多行注释（/* ... * / 包裹）
 * 作用：解析JSON时忽略多行注释，提升兼容性
 * 参数：input - 指向JSON字符串的指针（会修改指针位置）
 */
static void skip_multiline_comment(char **input)
{
    *input += static_strlen("/*");

    for (; (*input)[0] != '\0'; ++(*input))
    {
        if (((*input)[0] == '*') && ((*input)[1] == '/'))
        {
            *input += static_strlen("*/");
            return;
        }
    }
}

static void minify_string(char **input, char **output) {
    (*output)[0] = (*input)[0];
    *input += static_strlen("\"");
    *output += static_strlen("\"");


    for (; (*input)[0] != '\0'; (void)++(*input), ++(*output)) {
        (*output)[0] = (*input)[0];

        if ((*input)[0] == '\"') {
            (*output)[0] = '\"';
            *input += static_strlen("\"");
            *output += static_strlen("\"");
            return;
        } else if (((*input)[0] == '\\') && ((*input)[1] == '\"')) {
            (*output)[1] = (*input)[1];
            *input += static_strlen("\"");
            *output += static_strlen("\"");
        }
    }
}

CJSON_PUBLIC(void) cJSON_Minify(char *json)
{
    char *into = json;

    if (json == NULL)
    {
        return;
    }

    while (json[0] != '\0')
    {
        switch (json[0])
        {
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                json++;
                break;

            case '/':
                if (json[1] == '/')
                {
                    skip_oneline_comment(&json);
                }
                else if (json[1] == '*')
                {
                    skip_multiline_comment(&json);
                } else {
                    json++;
                }
                break;

            case '\"':
                minify_string(&json, (char**)&into);
                break;

            default:
                into[0] = json[0];
                json++;
                into++;
        }
    }

    /* and null-terminate. */
    *into = '\0';
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsInvalid(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Invalid;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsFalse(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_False;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsTrue(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xff) == cJSON_True;
}


CJSON_PUBLIC(cJSON_bool) cJSON_IsBool(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & (cJSON_True | cJSON_False)) != 0;
}
CJSON_PUBLIC(cJSON_bool) cJSON_IsNull(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_NULL;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsNumber(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Number;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsString(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_String;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsArray(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Array;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsObject(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Object;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsRaw(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Raw;
}

CJSON_PUBLIC(cJSON_bool) cJSON_Compare(const cJSON * const a, const cJSON * const b, const cJSON_bool case_sensitive)
{
    if ((a == NULL) || (b == NULL) || ((a->type & 0xFF) != (b->type & 0xFF)))
    {
        return false;
    }

    /* check if type is valid */
    switch (a->type & 0xFF)
    {
        case cJSON_False:
        case cJSON_True:
        case cJSON_NULL:
        case cJSON_Number:
        case cJSON_String:
        case cJSON_Raw:
        case cJSON_Array:
        case cJSON_Object:
            break;

        default:
            return false;
    }

    /* identical objects are equal */
    if (a == b)
    {
        return true;
    }

    switch (a->type & 0xFF)
    {
        /* in these cases and equal type is enough */
        case cJSON_False:
        case cJSON_True:
        case cJSON_NULL:
            return true;

        case cJSON_Number:
            if (compare_double(a->valuedouble, b->valuedouble))
            {
                return true;
            }
            return false;

        case cJSON_String:
        case cJSON_Raw:
            if ((a->valuestring == NULL) || (b->valuestring == NULL))
            {
                return false;
            }
            if (strcmp(a->valuestring, b->valuestring) == 0)
            {
                return true;
            }

            return false;

        case cJSON_Array:
        {
            cJSON *a_element = a->child;
            cJSON *b_element = b->child;

            for (; (a_element != NULL) && (b_element != NULL);)
            {
                if (!cJSON_Compare(a_element, b_element, case_sensitive))
                {
                    return false;
                }

                a_element = a_element->next;
                b_element = b_element->next;
            }

            /* one of the arrays is longer than the other */
            if (a_element != b_element) {
                return false;
            }

            return true;
        }

        case cJSON_Object:
        {
            cJSON *a_element = NULL;
            cJSON *b_element = NULL;
            cJSON_ArrayForEach(a_element, a)
            {
                /* TODO This has O(n^2) runtime, which is horrible! */
                b_element = get_object_item(b, a_element->string, case_sensitive);
                if (b_element == NULL)
                {
                    return false;
                }

                if (!cJSON_Compare(a_element, b_element, case_sensitive))
                {
                    return false;
                }
            }

            /* doing this twice, once on a and b to prevent true comparison if a subset of b
             * TODO: Do this the proper way, this is just a fix for now */
            cJSON_ArrayForEach(b_element, b)
            {
                a_element = get_object_item(a, b_element->string, case_sensitive);
                if (a_element == NULL)
                {
                    return false;
                }

                if (!cJSON_Compare(b_element, a_element, case_sensitive))
                {
                    return false;
                }
            }

            return true;
        }

        default:
            return false;
    }
}

CJSON_PUBLIC(void *) cJSON_malloc(size_t size)
{
    return global_hooks.allocate(size);
}

CJSON_PUBLIC(void) cJSON_free(void *object)
{
    global_hooks.deallocate(object);
    object = NULL;
}
