#include <alloc_hook.h>



int main() {
    alloc_hook_verbose_message("main()\n");
    alloc_hook_verbose_message("scope begin\n");
    {
        alloc_hook_verbose_message("scope inside begin\n");

        alloc_hook_verbose_message("scope inside end\n");
    }
    alloc_hook_verbose_message("scope end\n");
    return 0;
}
