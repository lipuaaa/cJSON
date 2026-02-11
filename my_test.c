#include <stdio.h>
#include "cJSON.h"

int main() {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "message", "Hello Ubuntu 24!");
    printf("%s\n", cJSON_Print(root));
    cJSON_Delete(root);
    return 0;
}