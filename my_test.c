#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"

int main() {
    /* 创建一个测试用的 JSON 对象 */
    cJSON *root = cJSON_CreateObject();
    cJSON *course = cJSON_CreateArray();
    
    cJSON_AddStringToObject(root, "name", "zhangsan");
    cJSON_AddNumberToObject(root, "age", 18);
    
    cJSON_AddItemToArray(course, cJSON_CreateString("aaa"));
    cJSON_AddItemToArray(course, cJSON_CreateString("bbb"));
    cJSON_AddItemToObject(root, "courses", course);

    /* --- 测试 1：官方原本的打印 (你会发现它是用 \t 的) --- */
    printf("=========== 官方自带打印 ===========\n");
    char *official_str = cJSON_Print(root);
    printf("%s\n\n", official_str);
    free(official_str);

    /* --- 测试 2：你的美化打印 (2个空格缩进) --- */
    printf("=========== 我的2空格美化打印 ===========\n");
    char *my_str_2 = cJSON_PrintPretty(root, 2);
    printf("%s\n\n", my_str_2);
    free(my_str_2);

    /* --- 测试 3：你的美化打印 (4个空格缩进) --- */
    printf("=========== 我的4空格美化打印 ===========\n");
    char *my_str_4 = cJSON_PrintPretty(root, 4);
    printf("%s\n\n", my_str_4);
    free(my_str_4);

    cJSON_Delete(root);
    return 0;
}