#include "enp/model/CLibrary.h"

#include <cstring>

namespace enp::model {

namespace {

using K = SymbolKind;

// Standard-library names worth suggesting: the ones code is written with every day, each with the
// header that declares it and the standards that have it. A C function is in both languages:
// <cstdio> (std::printf) in C++, <stdio.h> in C. Members are listed under their class.
const LibraryName kNames[] = {
    // ---- C library (also std:: in C++ through <c...>) -----------------------------------------
    {"printf", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "int printf(const char* format, ...)"},
    {"fprintf", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "int fprintf(FILE* stream, const char* format, ...)"},
    {"sprintf", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "int sprintf(char* buffer, const char* format, ...)"},
    {"snprintf", "std", "cstdio", "stdio.h", K::Function, 2011, 1999, "int snprintf(char* buffer, size_t size, const char* format, ...)"},
    {"puts", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "int puts(const char* text)"},
    {"fputs", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "int fputs(const char* text, FILE* stream)"},
    {"fgets", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "char* fgets(char* buffer, int size, FILE* stream)"},
    {"scanf", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "int scanf(const char* format, ...)"},
    {"sscanf", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "int sscanf(const char* text, const char* format, ...)"},
    {"fopen", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "FILE* fopen(const char* path, const char* mode)"},
    {"fclose", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "int fclose(FILE* stream)"},
    {"fread", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "size_t fread(void* buffer, size_t size, size_t count, FILE* stream)"},
    {"fwrite", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "size_t fwrite(const void* data, size_t size, size_t count, FILE* stream)"},
    {"fseek", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "int fseek(FILE* stream, long offset, int origin)"},
    {"ftell", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "long ftell(FILE* stream)"},
    {"fflush", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "int fflush(FILE* stream)"},
    {"perror", "std", "cstdio", "stdio.h", K::Function, 1998, 1989, "void perror(const char* text)"},
    {"FILE", "std", "cstdio", "stdio.h", K::Type, 1998, 1989, "struct FILE"},
    {"EOF", "", "cstdio", "stdio.h", K::Macro, 1998, 1989, "#define EOF (-1)"},
    {"stdout", "", "cstdio", "stdio.h", K::Macro, 1998, 1989, "FILE* stdout"},
    {"stderr", "", "cstdio", "stdio.h", K::Macro, 1998, 1989, "FILE* stderr"},
    {"stdin", "", "cstdio", "stdio.h", K::Macro, 1998, 1989, "FILE* stdin"},
    {"malloc", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "void* malloc(size_t size)"},
    {"calloc", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "void* calloc(size_t count, size_t size)"},
    {"realloc", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "void* realloc(void* block, size_t size)"},
    {"free", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "void free(void* block)"},
    {"aligned_alloc", "std", "cstdlib", "stdlib.h", K::Function, 2017, 2011, "void* aligned_alloc(size_t alignment, size_t size)"},
    {"atoi", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "int atoi(const char* text)"},
    {"strtol", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "long strtol(const char* text, char** end, int base)"},
    {"strtod", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "double strtod(const char* text, char** end)"},
    {"qsort", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "void qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*))"},
    {"bsearch", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "void* bsearch(const void* key, const void* base, size_t count, size_t size, int (*compare)(const void*, const void*))"},
    {"abs", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "int abs(int value)"},
    {"rand", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "int rand(void)"},
    {"srand", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "void srand(unsigned seed)"},
    {"exit", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "void exit(int status)"},
    {"abort", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "void abort(void)"},
    {"getenv", "std", "cstdlib", "stdlib.h", K::Function, 1998, 1989, "char* getenv(const char* name)"},
    {"EXIT_SUCCESS", "", "cstdlib", "stdlib.h", K::Macro, 1998, 1989, "#define EXIT_SUCCESS 0"},
    {"EXIT_FAILURE", "", "cstdlib", "stdlib.h", K::Macro, 1998, 1989, "#define EXIT_FAILURE 1"},
    {"strlen", "std", "cstring", "string.h", K::Function, 1998, 1989, "size_t strlen(const char* text)"},
    {"strcmp", "std", "cstring", "string.h", K::Function, 1998, 1989, "int strcmp(const char* a, const char* b)"},
    {"strncmp", "std", "cstring", "string.h", K::Function, 1998, 1989, "int strncmp(const char* a, const char* b, size_t count)"},
    {"strcpy", "std", "cstring", "string.h", K::Function, 1998, 1989, "char* strcpy(char* to, const char* from)"},
    {"strncpy", "std", "cstring", "string.h", K::Function, 1998, 1989, "char* strncpy(char* to, const char* from, size_t count)"},
    {"strcat", "std", "cstring", "string.h", K::Function, 1998, 1989, "char* strcat(char* to, const char* from)"},
    {"strchr", "std", "cstring", "string.h", K::Function, 1998, 1989, "char* strchr(const char* text, int c)"},
    {"strrchr", "std", "cstring", "string.h", K::Function, 1998, 1989, "char* strrchr(const char* text, int c)"},
    {"strstr", "std", "cstring", "string.h", K::Function, 1998, 1989, "char* strstr(const char* text, const char* part)"},
    {"strdup", "", nullptr, "string.h", K::Function, 0, 2023, "char* strdup(const char* text)"},
    {"memcpy", "std", "cstring", "string.h", K::Function, 1998, 1989, "void* memcpy(void* to, const void* from, size_t size)"},
    {"memmove", "std", "cstring", "string.h", K::Function, 1998, 1989, "void* memmove(void* to, const void* from, size_t size)"},
    {"memset", "std", "cstring", "string.h", K::Function, 1998, 1989, "void* memset(void* block, int value, size_t size)"},
    {"memcmp", "std", "cstring", "string.h", K::Function, 1998, 1989, "int memcmp(const void* a, const void* b, size_t size)"},
    {"sqrt", "std", "cmath", "math.h", K::Function, 1998, 1989, "double sqrt(double x)"},
    {"pow", "std", "cmath", "math.h", K::Function, 1998, 1989, "double pow(double base, double exponent)"},
    {"fabs", "std", "cmath", "math.h", K::Function, 1998, 1989, "double fabs(double x)"},
    {"floor", "std", "cmath", "math.h", K::Function, 1998, 1989, "double floor(double x)"},
    {"ceil", "std", "cmath", "math.h", K::Function, 1998, 1989, "double ceil(double x)"},
    {"round", "std", "cmath", "math.h", K::Function, 2011, 1999, "double round(double x)"},
    {"sin", "std", "cmath", "math.h", K::Function, 1998, 1989, "double sin(double x)"},
    {"cos", "std", "cmath", "math.h", K::Function, 1998, 1989, "double cos(double x)"},
    {"tan", "std", "cmath", "math.h", K::Function, 1998, 1989, "double tan(double x)"},
    {"atan2", "std", "cmath", "math.h", K::Function, 1998, 1989, "double atan2(double y, double x)"},
    {"exp", "std", "cmath", "math.h", K::Function, 1998, 1989, "double exp(double x)"},
    {"log", "std", "cmath", "math.h", K::Function, 1998, 1989, "double log(double x)"},
    {"log2", "std", "cmath", "math.h", K::Function, 2011, 1999, "double log2(double x)"},
    {"hypot", "std", "cmath", "math.h", K::Function, 2011, 1999, "double hypot(double x, double y)"},
    {"fmod", "std", "cmath", "math.h", K::Function, 1998, 1989, "double fmod(double x, double y)"},
    {"isnan", "std", "cmath", "math.h", K::Function, 2011, 1999, "bool isnan(double x)"},
    {"lerp", "std", "cmath", nullptr, K::Function, 2020, 0, "double lerp(double a, double b, double t)"},
    {"time", "std", "ctime", "time.h", K::Function, 1998, 1989, "time_t time(time_t* now)"},
    {"clock", "std", "ctime", "time.h", K::Function, 1998, 1989, "clock_t clock(void)"},
    {"strftime", "std", "ctime", "time.h", K::Function, 1998, 1989, "size_t strftime(char* buffer, size_t size, const char* format, const struct tm* time)"},
    {"assert", "", "cassert", "assert.h", K::Macro, 1998, 1989, "#define assert(condition)"},
    {"static_assert", "", nullptr, "assert.h", K::Macro, 0, 2011, "#define static_assert _Static_assert"},
    {"isdigit", "std", "cctype", "ctype.h", K::Function, 1998, 1989, "int isdigit(int c)"},
    {"isalpha", "std", "cctype", "ctype.h", K::Function, 1998, 1989, "int isalpha(int c)"},
    {"isspace", "std", "cctype", "ctype.h", K::Function, 1998, 1989, "int isspace(int c)"},
    {"toupper", "std", "cctype", "ctype.h", K::Function, 1998, 1989, "int toupper(int c)"},
    {"tolower", "std", "cctype", "ctype.h", K::Function, 1998, 1989, "int tolower(int c)"},
    {"errno", "", "cerrno", "errno.h", K::Macro, 1998, 1989, "int errno"},
    {"size_t", "std", "cstddef", "stddef.h", K::Type, 1998, 1989, "typedef unsigned size_t"},
    {"ptrdiff_t", "std", "cstddef", "stddef.h", K::Type, 1998, 1989, "typedef signed ptrdiff_t"},
    {"NULL", "", "cstddef", "stddef.h", K::Macro, 1998, 1989, "#define NULL"},
    {"offsetof", "", "cstddef", "stddef.h", K::Macro, 1998, 1989, "#define offsetof(type, member)"},
    {"nullptr_t", "std", "cstddef", "stddef.h", K::Type, 2011, 2023, "typedef decltype(nullptr) nullptr_t"},
    {"byte", "std", "cstddef", nullptr, K::Type, 2017, 0, "enum class byte : unsigned char"},
    {"int8_t", "std", "cstdint", "stdint.h", K::Type, 2011, 1999, "typedef signed char int8_t"},
    {"int16_t", "std", "cstdint", "stdint.h", K::Type, 2011, 1999, "typedef short int16_t"},
    {"int32_t", "std", "cstdint", "stdint.h", K::Type, 2011, 1999, "typedef int int32_t"},
    {"int64_t", "std", "cstdint", "stdint.h", K::Type, 2011, 1999, "typedef long long int64_t"},
    {"uint8_t", "std", "cstdint", "stdint.h", K::Type, 2011, 1999, "typedef unsigned char uint8_t"},
    {"uint16_t", "std", "cstdint", "stdint.h", K::Type, 2011, 1999, "typedef unsigned short uint16_t"},
    {"uint32_t", "std", "cstdint", "stdint.h", K::Type, 2011, 1999, "typedef unsigned uint32_t"},
    {"uint64_t", "std", "cstdint", "stdint.h", K::Type, 2011, 1999, "typedef unsigned long long uint64_t"},
    {"intptr_t", "std", "cstdint", "stdint.h", K::Type, 2011, 1999, "typedef long intptr_t"},
    {"uintptr_t", "std", "cstdint", "stdint.h", K::Type, 2011, 1999, "typedef unsigned long uintptr_t"},
    {"INT32_MAX", "", "cstdint", "stdint.h", K::Macro, 2011, 1999, "#define INT32_MAX 2147483647"},
    {"SIZE_MAX", "", "cstdint", "stdint.h", K::Macro, 2011, 1999, "#define SIZE_MAX"},
    {"INT_MAX", "", "climits", "limits.h", K::Macro, 1998, 1989, "#define INT_MAX"},
    {"INT_MIN", "", "climits", "limits.h", K::Macro, 1998, 1989, "#define INT_MIN"},
    {"CHAR_BIT", "", "climits", "limits.h", K::Macro, 1998, 1989, "#define CHAR_BIT 8"},
    {"DBL_MAX", "", "cfloat", "float.h", K::Macro, 1998, 1989, "#define DBL_MAX"},
    {"FLT_EPSILON", "", "cfloat", "float.h", K::Macro, 1998, 1989, "#define FLT_EPSILON"},
    {"va_list", "std", "cstdarg", "stdarg.h", K::Type, 1998, 1989, "typedef va_list"},
    {"va_start", "", "cstdarg", "stdarg.h", K::Macro, 1998, 1989, "#define va_start(list, last)"},
    {"va_end", "", "cstdarg", "stdarg.h", K::Macro, 1998, 1989, "#define va_end(list)"},
    {"bool", "", nullptr, "stdbool.h", K::Macro, 0, 1999, "#define bool _Bool"},
    {"true", "", nullptr, "stdbool.h", K::Macro, 0, 1999, "#define true 1"},
    {"false", "", nullptr, "stdbool.h", K::Macro, 0, 1999, "#define false 0"},
    {"atomic_int", "", nullptr, "stdatomic.h", K::Type, 0, 2011, "typedef _Atomic int atomic_int"},
    {"atomic_fetch_add", "", nullptr, "stdatomic.h", K::Function, 0, 2011, "C atomic_fetch_add(volatile A* object, M operand)"},
    {"thrd_create", "", nullptr, "threads.h", K::Function, 0, 2011, "int thrd_create(thrd_t* thread, thrd_start_t function, void* argument)"},
    {"mtx_lock", "", nullptr, "threads.h", K::Function, 0, 2011, "int mtx_lock(mtx_t* mutex)"},
    {"ckd_add", "", nullptr, "stdckdint.h", K::Macro, 0, 2023, "bool ckd_add(type* result, a, b)"},
    {"ckd_mul", "", nullptr, "stdckdint.h", K::Macro, 0, 2023, "bool ckd_mul(type* result, a, b)"},
    {"stdc_count_ones", "", nullptr, "stdbit.h", K::Function, 0, 2023, "unsigned stdc_count_ones(value)"},

    // ---- C++ library ---------------------------------------------------------------------------
    {"cout", "std", "iostream", nullptr, K::Variable, 1998, 0, "extern std::ostream cout"},
    {"cerr", "std", "iostream", nullptr, K::Variable, 1998, 0, "extern std::ostream cerr"},
    {"cin", "std", "iostream", nullptr, K::Variable, 1998, 0, "extern std::istream cin"},
    {"endl", "std", "ostream", nullptr, K::Function, 1998, 0, "std::ostream& endl(std::ostream& out)"},
    {"getline", "std", "string", nullptr, K::Function, 1998, 0, "std::istream& getline(std::istream& in, std::string& line, char delimiter = '\\n')"},
    {"ifstream", "std", "fstream", nullptr, K::Type, 1998, 0, "class ifstream"},
    {"ofstream", "std", "fstream", nullptr, K::Type, 1998, 0, "class ofstream"},
    {"stringstream", "std", "sstream", nullptr, K::Type, 1998, 0, "class stringstream"},
    {"ostringstream", "std", "sstream", nullptr, K::Type, 1998, 0, "class ostringstream"},
    {"string", "std", "string", nullptr, K::Type, 1998, 0, "class basic_string<char>"},
    {"wstring", "std", "string", nullptr, K::Type, 1998, 0, "class basic_string<wchar_t>"},
    {"to_string", "std", "string", nullptr, K::Function, 2011, 0, "std::string to_string(T value)"},
    {"stoi", "std", "string", nullptr, K::Function, 2011, 0, "int stoi(const std::string& text, size_t* end = nullptr, int base = 10)"},
    {"stod", "std", "string", nullptr, K::Function, 2011, 0, "double stod(const std::string& text, size_t* end = nullptr)"},
    {"string_view", "std", "string_view", nullptr, K::Type, 2017, 0, "class basic_string_view<char>"},
    {"vector", "std", "vector", nullptr, K::Type, 1998, 0, "template <class T, class Allocator> class vector"},
    {"array", "std", "array", nullptr, K::Type, 2011, 0, "template <class T, size_t N> struct array"},
    {"deque", "std", "deque", nullptr, K::Type, 1998, 0, "template <class T> class deque"},
    {"list", "std", "list", nullptr, K::Type, 1998, 0, "template <class T> class list"},
    {"forward_list", "std", "forward_list", nullptr, K::Type, 2011, 0, "template <class T> class forward_list"},
    {"map", "std", "map", nullptr, K::Type, 1998, 0, "template <class Key, class T> class map"},
    {"multimap", "std", "map", nullptr, K::Type, 1998, 0, "template <class Key, class T> class multimap"},
    {"set", "std", "set", nullptr, K::Type, 1998, 0, "template <class Key> class set"},
    {"unordered_map", "std", "unordered_map", nullptr, K::Type, 2011, 0, "template <class Key, class T> class unordered_map"},
    {"unordered_set", "std", "unordered_set", nullptr, K::Type, 2011, 0, "template <class Key> class unordered_set"},
    {"flat_map", "std", "flat_map", nullptr, K::Type, 2023, 0, "template <class Key, class T> class flat_map"},
    {"stack", "std", "stack", nullptr, K::Type, 1998, 0, "template <class T> class stack"},
    {"queue", "std", "queue", nullptr, K::Type, 1998, 0, "template <class T> class queue"},
    {"priority_queue", "std", "queue", nullptr, K::Type, 1998, 0, "template <class T> class priority_queue"},
    {"span", "std", "span", nullptr, K::Type, 2020, 0, "template <class T, size_t Extent> class span"},
    {"mdspan", "std", "mdspan", nullptr, K::Type, 2023, 0, "template <class T, class Extents> class mdspan"},
    {"pair", "std", "utility", nullptr, K::Type, 1998, 0, "template <class T1, class T2> struct pair"},
    {"make_pair", "std", "utility", nullptr, K::Function, 1998, 0, "std::pair<T1, T2> make_pair(T1 first, T2 second)"},
    {"move", "std", "utility", nullptr, K::Function, 2011, 0, "T&& move(T&& value)"},
    {"forward", "std", "utility", nullptr, K::Function, 2011, 0, "T&& forward(T& value)"},
    {"swap", "std", "utility", nullptr, K::Function, 1998, 0, "void swap(T& a, T& b)"},
    {"exchange", "std", "utility", nullptr, K::Function, 2014, 0, "T exchange(T& target, U&& value)"},
    {"tuple", "std", "tuple", nullptr, K::Type, 2011, 0, "template <class... T> class tuple"},
    {"make_tuple", "std", "tuple", nullptr, K::Function, 2011, 0, "std::tuple<T...> make_tuple(T&&... values)"},
    {"get", "std", "tuple", nullptr, K::Function, 2011, 0, "T& get<I>(std::tuple<T...>& tuple)"},
    {"optional", "std", "optional", nullptr, K::Type, 2017, 0, "template <class T> class optional"},
    {"nullopt", "std", "optional", nullptr, K::Constant, 2017, 0, "inline constexpr nullopt_t nullopt"},
    {"make_optional", "std", "optional", nullptr, K::Function, 2017, 0, "std::optional<T> make_optional(T&& value)"},
    {"variant", "std", "variant", nullptr, K::Type, 2017, 0, "template <class... T> class variant"},
    {"visit", "std", "variant", nullptr, K::Function, 2017, 0, "R visit(Visitor&& visitor, Variants&&... variants)"},
    {"holds_alternative", "std", "variant", nullptr, K::Function, 2017, 0, "bool holds_alternative<T>(const std::variant<...>& v)"},
    {"any", "std", "any", nullptr, K::Type, 2017, 0, "class any"},
    {"expected", "std", "expected", nullptr, K::Type, 2023, 0, "template <class T, class E> class expected"},
    {"unexpected", "std", "expected", nullptr, K::Type, 2023, 0, "template <class E> class unexpected"},
    {"unique_ptr", "std", "memory", nullptr, K::Type, 2011, 0, "template <class T, class Deleter> class unique_ptr"},
    {"shared_ptr", "std", "memory", nullptr, K::Type, 2011, 0, "template <class T> class shared_ptr"},
    {"weak_ptr", "std", "memory", nullptr, K::Type, 2011, 0, "template <class T> class weak_ptr"},
    {"make_unique", "std", "memory", nullptr, K::Function, 2014, 0, "std::unique_ptr<T> make_unique(Args&&... args)"},
    {"make_shared", "std", "memory", nullptr, K::Function, 2011, 0, "std::shared_ptr<T> make_shared(Args&&... args)"},
    {"function", "std", "functional", nullptr, K::Type, 2011, 0, "template <class R(Args...)> class function"},
    {"bind", "std", "functional", nullptr, K::Function, 2011, 0, "auto bind(F&& f, Args&&... args)"},
    {"invoke", "std", "functional", nullptr, K::Function, 2017, 0, "auto invoke(F&& f, Args&&... args)"},
    {"hash", "std", "functional", nullptr, K::Type, 2011, 0, "template <class T> struct hash"},
    {"sort", "std", "algorithm", nullptr, K::Function, 1998, 0, "void sort(RandomIt first, RandomIt last, Compare compare = {})"},
    {"stable_sort", "std", "algorithm", nullptr, K::Function, 1998, 0, "void stable_sort(RandomIt first, RandomIt last, Compare compare = {})"},
    {"find", "std", "algorithm", nullptr, K::Function, 1998, 0, "InputIt find(InputIt first, InputIt last, const T& value)"},
    {"find_if", "std", "algorithm", nullptr, K::Function, 1998, 0, "InputIt find_if(InputIt first, InputIt last, Predicate predicate)"},
    {"count_if", "std", "algorithm", nullptr, K::Function, 1998, 0, "size_t count_if(InputIt first, InputIt last, Predicate predicate)"},
    {"any_of", "std", "algorithm", nullptr, K::Function, 2011, 0, "bool any_of(InputIt first, InputIt last, Predicate predicate)"},
    {"all_of", "std", "algorithm", nullptr, K::Function, 2011, 0, "bool all_of(InputIt first, InputIt last, Predicate predicate)"},
    {"copy", "std", "algorithm", nullptr, K::Function, 1998, 0, "OutputIt copy(InputIt first, InputIt last, OutputIt to)"},
    {"transform", "std", "algorithm", nullptr, K::Function, 1998, 0, "OutputIt transform(InputIt first, InputIt last, OutputIt to, Function f)"},
    {"remove_if", "std", "algorithm", nullptr, K::Function, 1998, 0, "ForwardIt remove_if(ForwardIt first, ForwardIt last, Predicate predicate)"},
    {"reverse", "std", "algorithm", nullptr, K::Function, 1998, 0, "void reverse(BidirIt first, BidirIt last)"},
    {"unique", "std", "algorithm", nullptr, K::Function, 1998, 0, "ForwardIt unique(ForwardIt first, ForwardIt last)"},
    {"lower_bound", "std", "algorithm", nullptr, K::Function, 1998, 0, "ForwardIt lower_bound(ForwardIt first, ForwardIt last, const T& value)"},
    {"binary_search", "std", "algorithm", nullptr, K::Function, 1998, 0, "bool binary_search(ForwardIt first, ForwardIt last, const T& value)"},
    {"min", "std", "algorithm", nullptr, K::Function, 1998, 0, "const T& min(const T& a, const T& b)"},
    {"max", "std", "algorithm", nullptr, K::Function, 1998, 0, "const T& max(const T& a, const T& b)"},
    {"clamp", "std", "algorithm", nullptr, K::Function, 2017, 0, "const T& clamp(const T& value, const T& low, const T& high)"},
    {"min_element", "std", "algorithm", nullptr, K::Function, 1998, 0, "ForwardIt min_element(ForwardIt first, ForwardIt last)"},
    {"max_element", "std", "algorithm", nullptr, K::Function, 1998, 0, "ForwardIt max_element(ForwardIt first, ForwardIt last)"},
    {"fill", "std", "algorithm", nullptr, K::Function, 1998, 0, "void fill(ForwardIt first, ForwardIt last, const T& value)"},
    {"for_each", "std", "algorithm", nullptr, K::Function, 1998, 0, "Function for_each(InputIt first, InputIt last, Function f)"},
    {"accumulate", "std", "numeric", nullptr, K::Function, 1998, 0, "T accumulate(InputIt first, InputIt last, T init)"},
    {"iota", "std", "numeric", nullptr, K::Function, 2011, 0, "void iota(ForwardIt first, ForwardIt last, T value)"},
    {"reduce", "std", "numeric", nullptr, K::Function, 2017, 0, "T reduce(InputIt first, InputIt last, T init)"},
    {"gcd", "std", "numeric", nullptr, K::Function, 2017, 0, "auto gcd(M a, N b)"},
    {"numeric_limits", "std", "limits", nullptr, K::Type, 1998, 0, "template <class T> class numeric_limits"},
    {"begin", "std", "iterator", nullptr, K::Function, 2011, 0, "auto begin(C& container)"},
    {"end", "std", "iterator", nullptr, K::Function, 2011, 0, "auto end(C& container)"},
    {"size", "std", "iterator", nullptr, K::Function, 2017, 0, "auto size(const C& container)"},
    {"back_inserter", "std", "iterator", nullptr, K::Function, 1998, 0, "std::back_insert_iterator<C> back_inserter(C& container)"},
    {"thread", "std", "thread", nullptr, K::Type, 2011, 0, "class thread"},
    {"jthread", "std", "thread", nullptr, K::Type, 2020, 0, "class jthread"},
    {"mutex", "std", "mutex", nullptr, K::Type, 2011, 0, "class mutex"},
    {"lock_guard", "std", "mutex", nullptr, K::Type, 2011, 0, "template <class Mutex> class lock_guard"},
    {"unique_lock", "std", "mutex", nullptr, K::Type, 2011, 0, "template <class Mutex> class unique_lock"},
    {"scoped_lock", "std", "mutex", nullptr, K::Type, 2017, 0, "template <class... Mutexes> class scoped_lock"},
    {"condition_variable", "std", "condition_variable", nullptr, K::Type, 2011, 0, "class condition_variable"},
    {"atomic", "std", "atomic", nullptr, K::Type, 2011, 0, "template <class T> struct atomic"},
    {"async", "std", "future", nullptr, K::Function, 2011, 0, "std::future<R> async(F&& f, Args&&... args)"},
    {"future", "std", "future", nullptr, K::Type, 2011, 0, "template <class T> class future"},
    {"chrono", "std", "chrono", nullptr, K::Namespace, 2011, 0, "namespace std::chrono"},
    {"filesystem", "std", "filesystem", nullptr, K::Namespace, 2017, 0, "namespace std::filesystem"},
    {"ranges", "std", "ranges", nullptr, K::Namespace, 2020, 0, "namespace std::ranges"},
    {"views", "std", "ranges", nullptr, K::Namespace, 2020, 0, "namespace std::views"},
    {"format", "std", "format", nullptr, K::Function, 2020, 0, "std::string format(std::format_string<Args...> format, Args&&... args)"},
    {"print", "std", "print", nullptr, K::Function, 2023, 0, "void print(std::format_string<Args...> format, Args&&... args)"},
    {"println", "std", "print", nullptr, K::Function, 2023, 0, "void println(std::format_string<Args...> format, Args&&... args)"},
    {"regex", "std", "regex", nullptr, K::Type, 2011, 0, "class basic_regex<char>"},
    {"regex_search", "std", "regex", nullptr, K::Function, 2011, 0, "bool regex_search(const std::string& text, std::smatch& match, const std::regex& pattern)"},
    {"mt19937", "std", "random", nullptr, K::Type, 2011, 0, "class mersenne_twister_engine<...>"},
    {"uniform_int_distribution", "std", "random", nullptr, K::Type, 2011, 0, "template <class Int> class uniform_int_distribution"},
    {"runtime_error", "std", "stdexcept", nullptr, K::Type, 1998, 0, "class runtime_error : public std::exception"},
    {"invalid_argument", "std", "stdexcept", nullptr, K::Type, 1998, 0, "class invalid_argument : public std::logic_error"},
    {"exception", "std", "exception", nullptr, K::Type, 1998, 0, "class exception"},
    {"is_same_v", "std", "type_traits", nullptr, K::Constant, 2017, 0, "template <class T, class U> constexpr bool is_same_v"},
    {"enable_if_t", "std", "type_traits", nullptr, K::Type, 2014, 0, "template <bool B, class T> using enable_if_t"},
    {"decay_t", "std", "type_traits", nullptr, K::Type, 2014, 0, "template <class T> using decay_t"},
    {"integral", "std", "concepts", nullptr, K::Type, 2020, 0, "template <class T> concept integral"},
    {"same_as", "std", "concepts", nullptr, K::Type, 2020, 0, "template <class T, class U> concept same_as"},
    {"bit_cast", "std", "bit", nullptr, K::Function, 2020, 0, "To bit_cast(const From& from)"},
    {"popcount", "std", "bit", nullptr, K::Function, 2020, 0, "int popcount(T value)"},
    {"source_location", "std", "source_location", nullptr, K::Type, 2020, 0, "struct source_location"},
    {"stacktrace", "std", "stacktrace", nullptr, K::Type, 2023, 0, "class basic_stacktrace<...>"},
    {"unreachable", "std", "utility", nullptr, K::Function, 2023, 0, "[[noreturn]] void unreachable()"},
    {"generator", "std", "generator", nullptr, K::Type, 2023, 0, "template <class Ref> class generator"},

    // ---- Members of the everyday classes (after "." or "->") ----------------------------------
    {"push_back", "vector", "vector", nullptr, K::Function, 1998, 0, "void push_back(const T& value)"},
    {"emplace_back", "vector", "vector", nullptr, K::Function, 2011, 0, "T& emplace_back(Args&&... args)"},
    {"pop_back", "vector", "vector", nullptr, K::Function, 1998, 0, "void pop_back()"},
    {"reserve", "vector", "vector", nullptr, K::Function, 1998, 0, "void reserve(size_t capacity)"},
    {"resize", "vector", "vector", nullptr, K::Function, 1998, 0, "void resize(size_t count)"},
    {"shrink_to_fit", "vector", "vector", nullptr, K::Function, 2011, 0, "void shrink_to_fit()"},
    {"capacity", "vector", "vector", nullptr, K::Function, 1998, 0, "size_t capacity() const"},
    {"data", "vector", "vector", nullptr, K::Function, 2011, 0, "T* data()"},
    {"front", "vector", "vector", nullptr, K::Function, 1998, 0, "T& front()"},
    {"back", "vector", "vector", nullptr, K::Function, 1998, 0, "T& back()"},
    {"empty", "vector", "vector", nullptr, K::Function, 1998, 0, "bool empty() const"},
    {"clear", "vector", "vector", nullptr, K::Function, 1998, 0, "void clear()"},
    {"insert", "vector", "vector", nullptr, K::Function, 1998, 0, "iterator insert(const_iterator at, const T& value)"},
    {"erase", "vector", "vector", nullptr, K::Function, 1998, 0, "iterator erase(const_iterator at)"},
    {"at", "vector", "vector", nullptr, K::Function, 1998, 0, "T& at(size_t index)"},
    {"c_str", "string", "string", nullptr, K::Function, 1998, 0, "const char* c_str() const"},
    {"substr", "string", "string", nullptr, K::Function, 1998, 0, "std::string substr(size_t position = 0, size_t count = npos) const"},
    {"append", "string", "string", nullptr, K::Function, 1998, 0, "std::string& append(const std::string& text)"},
    {"find_first_of", "string", "string", nullptr, K::Function, 1998, 0, "size_t find_first_of(const std::string& characters, size_t position = 0) const"},
    {"starts_with", "string", "string", nullptr, K::Function, 2020, 0, "bool starts_with(std::string_view prefix) const"},
    {"ends_with", "string", "string", nullptr, K::Function, 2020, 0, "bool ends_with(std::string_view suffix) const"},
    {"contains", "string", "string", nullptr, K::Function, 2023, 0, "bool contains(std::string_view part) const"},
    {"length", "string", "string", nullptr, K::Function, 1998, 0, "size_t length() const"},
    {"npos", "string", "string", nullptr, K::Constant, 1998, 0, "static constexpr size_t npos = size_t(-1)"},
    {"emplace", "map", "map", nullptr, K::Function, 2011, 0, "std::pair<iterator, bool> emplace(Args&&... args)"},
    {"try_emplace", "map", "map", nullptr, K::Function, 2017, 0, "std::pair<iterator, bool> try_emplace(const Key& key, Args&&... args)"},
    {"insert_or_assign", "map", "map", nullptr, K::Function, 2017, 0, "std::pair<iterator, bool> insert_or_assign(const Key& key, M&& value)"},
    {"count", "map", "map", nullptr, K::Function, 1998, 0, "size_t count(const Key& key) const"},
    {"has_value", "optional", "optional", nullptr, K::Function, 2017, 0, "bool has_value() const"},
    {"value", "optional", "optional", nullptr, K::Function, 2017, 0, "T& value()"},
    {"value_or", "optional", "optional", nullptr, K::Function, 2017, 0, "T value_or(U&& fallback) const"},
    {"reset", "unique_ptr", "memory", nullptr, K::Function, 2011, 0, "void reset(T* pointer = nullptr)"},
    {"release", "unique_ptr", "memory", nullptr, K::Function, 2011, 0, "T* release()"},
    {"lock", "weak_ptr", "memory", nullptr, K::Function, 2011, 0, "std::shared_ptr<T> lock() const"},
    {"join", "thread", "thread", nullptr, K::Function, 2011, 0, "void join()"},
    {"detach", "thread", "thread", nullptr, K::Function, 2011, 0, "void detach()"},
    {"load", "atomic", "atomic", nullptr, K::Function, 2011, 0, "T load(std::memory_order order = std::memory_order_seq_cst) const"},
    {"store", "atomic", "atomic", nullptr, K::Function, 2011, 0, "void store(T value, std::memory_order order = std::memory_order_seq_cst)"},
};

// Keywords and the standard that made each one a keyword (-1: not one in that language).
const KeywordSince kKeywords[] = {
    {"auto", 1998, 1989}, {"break", 1998, 1989}, {"case", 1998, 1989}, {"char", 1998, 1989}, {"const", 1998, 1989},
    {"continue", 1998, 1989}, {"default", 1998, 1989}, {"do", 1998, 1989}, {"double", 1998, 1989}, {"else", 1998, 1989},
    {"enum", 1998, 1989}, {"extern", 1998, 1989}, {"float", 1998, 1989}, {"for", 1998, 1989}, {"goto", 1998, 1989},
    {"if", 1998, 1989}, {"int", 1998, 1989}, {"long", 1998, 1989}, {"register", 1998, 1989}, {"return", 1998, 1989},
    {"short", 1998, 1989}, {"signed", 1998, 1989}, {"sizeof", 1998, 1989}, {"static", 1998, 1989}, {"struct", 1998, 1989},
    {"switch", 1998, 1989}, {"typedef", 1998, 1989}, {"union", 1998, 1989}, {"unsigned", 1998, 1989}, {"void", 1998, 1989},
    {"volatile", 1998, 1989}, {"while", 1998, 1989},
    // C99 / C11 / C23
    {"inline", 1998, 1999}, {"restrict", -1, 1999}, {"_Bool", -1, 1999}, {"_Complex", -1, 1999},
    {"_Alignas", -1, 2011}, {"_Alignof", -1, 2011}, {"_Atomic", -1, 2011}, {"_Generic", -1, 2011}, {"_Noreturn", -1, 2011},
    {"_Static_assert", -1, 2011}, {"_Thread_local", -1, 2011},
    {"bool", 1998, 2023}, {"true", 1998, 2023}, {"false", 1998, 2023}, {"nullptr", 2011, 2023}, {"constexpr", 2011, 2023},
    {"static_assert", 2011, 2023}, {"thread_local", 2011, 2023}, {"alignas", 2011, 2023}, {"alignof", 2011, 2023},
    {"typeof", -1, 2023}, {"typeof_unqual", -1, 2023},
    // C++
    {"asm", 1998, -1}, {"catch", 1998, -1}, {"class", 1998, -1}, {"const_cast", 1998, -1}, {"delete", 1998, -1},
    {"dynamic_cast", 1998, -1}, {"explicit", 1998, -1}, {"export", 1998, -1}, {"friend", 1998, -1}, {"mutable", 1998, -1},
    {"namespace", 1998, -1}, {"new", 1998, -1}, {"operator", 1998, -1}, {"private", 1998, -1}, {"protected", 1998, -1},
    {"public", 1998, -1}, {"reinterpret_cast", 1998, -1}, {"static_cast", 1998, -1}, {"template", 1998, -1}, {"this", 1998, -1},
    {"throw", 1998, -1}, {"try", 1998, -1}, {"typeid", 1998, -1}, {"typename", 1998, -1}, {"using", 1998, -1},
    {"virtual", 1998, -1}, {"wchar_t", 1998, -1},
    {"char16_t", 2011, -1}, {"char32_t", 2011, -1}, {"decltype", 2011, -1}, {"noexcept", 2011, -1}, {"override", 2011, -1},
    {"final", 2011, -1},
    {"char8_t", 2020, -1}, {"concept", 2020, -1}, {"requires", 2020, -1}, {"consteval", 2020, -1}, {"constinit", 2020, -1},
    {"co_await", 2020, -1}, {"co_return", 2020, -1}, {"co_yield", 2020, -1},
};

const int kCppStandards[] = {1998, 2003, 2011, 2014, 2017, 2020, 2023, 2026};
const int kCStandards[] = {1989, 1999, 2011, 2017, 2023};

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// The year of a two-digit C++ standard ("17" -> 2017, "98" -> 1998, "2a" -> 2020).
int cppYear(std::string_view digits)
{
    if (digits.size() < 2)
        return 0;
    if (digits == "2a")
        return 2020;
    if (digits == "2b")
        return 2023;
    if (digits == "2c")
        return 2026;
    if (!isDigit(digits[0]) || !isDigit(digits[1]))
        return 0;
    const int n = (digits[0] - '0') * 10 + (digits[1] - '0');
    switch (n) {
    case 98: return 1998;
    case 3: return 2003;
    case 11: case 14: case 17: case 20: case 23: case 26: return 2000 + n;
    default: return 0;
    }
}

int cYear(std::string_view digits)
{
    if (digits.size() < 2)
        return 0;
    if (digits == "2x")
        return 2023;
    if (!isDigit(digits[0]) || !isDigit(digits[1]))
        return 0;
    const int n = (digits[0] - '0') * 10 + (digits[1] - '0');
    switch (n) {
    case 89: case 90: return 1989;
    case 99: return 1999;
    case 11: return 2011;
    case 17: case 18: return 2017;
    case 23: return 2023;
    default: return 0;
    }
}

// The two characters after each occurrence of `marker` (skipping spaces), mapped by `year`; the
// last one that names a standard wins (a later line overrides).
template <class Year>
int findStandard(std::string_view text, std::string_view marker, Year year, bool skipSpaces)
{
    int found = 0;
    for (size_t at = text.find(marker); at != std::string_view::npos; at = text.find(marker, at + 1)) {
        size_t p = at + marker.size();
        while (skipSpaces && p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '"'))
            ++p;
        if (p + 2 <= text.size())
            if (const int y = year(text.substr(p, 2)))
                found = y;
    }
    return found;
}

} // namespace

const int* cppStandards(size_t& count)
{
    count = sizeof(kCppStandards) / sizeof(kCppStandards[0]);
    return kCppStandards;
}

const int* cStandards(size_t& count)
{
    count = sizeof(kCStandards) / sizeof(kCStandards[0]);
    return kCStandards;
}

const char* standardName(int year, bool c)
{
    if (c) {
        switch (year) {
        case 1989: return "C89";
        case 1999: return "C99";
        case 2011: return "C11";
        case 2017: return "C17";
        case 2023: return "C23";
        default: return "C";
        }
    }
    switch (year) {
    case 1998: return "C++98";
    case 2003: return "C++03";
    case 2011: return "C++11";
    case 2014: return "C++14";
    case 2017: return "C++17";
    case 2020: return "C++20";
    case 2023: return "C++23";
    case 2026: return "C++26";
    default: return "C++";
    }
}

const LibraryName* libraryNames(size_t& count)
{
    count = sizeof(kNames) / sizeof(kNames[0]);
    return kNames;
}

bool libraryNameAvailable(const LibraryName& n, bool c, int standard)
{
    const int since = c ? n.cSince : n.cppSince;
    return since > 0 && since <= standard && (c ? n.cHeader : n.header) != nullptr;
}

std::string_view libraryHeader(const LibraryName& n, bool c)
{
    const char* header = c ? n.cHeader : n.header;
    return header ? std::string_view(header) : std::string_view();
}

const LibraryName* findLibraryName(std::string_view name, bool c)
{
    const LibraryName* member = nullptr;
    for (const LibraryName& n : kNames) {
        if (name != n.name || (c ? n.cSince : n.cppSince) <= 0)
            continue;
        const std::string_view scope(n.scope);
        if (scope.empty() || scope == "std")
            return &n; // a namespace-level name first
        if (!member)
            member = &n;
    }
    return member;
}

const KeywordSince* keywordTable(size_t& count)
{
    count = sizeof(kKeywords) / sizeof(kKeywords[0]);
    return kKeywords;
}

int keywordSince(std::string_view word, bool c)
{
    for (const KeywordSince& k : kKeywords)
        if (word == k.word)
            return c ? k.cSince : k.cppSince;
    return -1;
}

bool isCSource(std::string_view path)
{
    return path.size() >= 2 && path.compare(path.size() - 2, 2, ".c") == 0; // ".C" (capital) is C++ by tradition
}

int detectCppStandard(std::string_view text)
{
    int found = 0;
    for (const int y : {findStandard(text, "CMAKE_CXX_STANDARD", cppYear, true), findStandard(text, "cxx_std_", cppYear, false),
                        findStandard(text, "-std=c++", cppYear, false), findStandard(text, "-std=gnu++", cppYear, false),
                        findStandard(text, "/std:c++", cppYear, false), findStandard(text, "cpp_std=c++", cppYear, false)})
        if (y)
            found = y;
    return found;
}

int detectCStandard(std::string_view text)
{
    int found = 0;
    for (const int y : {findStandard(text, "CMAKE_C_STANDARD", cYear, true), findStandard(text, "c_std_", cYear, false),
                        findStandard(text, "-std=c", cYear, false), findStandard(text, "-std=gnu", cYear, false),
                        findStandard(text, "/std:c", cYear, false), findStandard(text, "c_std=c", cYear, false)})
        if (y)
            found = y;
    return found;
}

namespace {

// One line: an #include, or nothing.
void includeOf(std::string_view l, int line, std::vector<Include>& out)
{
    size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t'))
        ++i;
    if (i >= l.size() || l[i] != '#')
        return;
    ++i;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t'))
        ++i;
    if (l.compare(i, 7, "include") != 0)
        return;
    i += 7;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t'))
        ++i;
    if (i < l.size() && (l[i] == '"' || l[i] == '<')) {
        const char close = l[i] == '"' ? '"' : '>';
        const size_t e = l.find(close, i + 1);
        if (e != std::string_view::npos && e > i + 1)
            out.push_back({std::string(l.substr(i + 1, e - i - 1)), close == '>', line});
    }
}

} // namespace

void scanIncludes(const TextBuffer& text, std::vector<Include>& out)
{
    for (int l = 0; l < text.lineCount(); ++l)
        includeOf(text.line(l), l, out);
}

void scanIncludes(std::string_view text, std::vector<Include>& out)
{
    int line = 0;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view l = text.substr(start, end - start);
        size_t i = 0;
        while (i < l.size() && (l[i] == ' ' || l[i] == '\t'))
            ++i;
        if (i < l.size() && l[i] == '#') {
            ++i;
            while (i < l.size() && (l[i] == ' ' || l[i] == '\t'))
                ++i;
            if (l.compare(i, 7, "include") == 0) {
                i += 7;
                while (i < l.size() && (l[i] == ' ' || l[i] == '\t'))
                    ++i;
                if (i < l.size() && (l[i] == '"' || l[i] == '<')) {
                    const char close = l[i] == '"' ? '"' : '>';
                    const size_t e = l.find(close, i + 1);
                    if (e != std::string_view::npos && e > i + 1)
                        out.push_back({std::string(l.substr(i + 1, e - i - 1)), close == '>', line});
                }
            }
        }
        ++line;
        start = end + 1;
    }
}

} // namespace enp::model
