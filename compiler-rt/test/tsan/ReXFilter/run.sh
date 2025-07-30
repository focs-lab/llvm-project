clang -fsanitize=thread -g -pthread needle_in_haystack_race.c -o bin/needle_in_haystack_race && TSAN_OPTIONS="enable_filter=1,verbosity=1" bin/needle_in_haystack_race 2>&1 | less
