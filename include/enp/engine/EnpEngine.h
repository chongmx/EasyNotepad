/* The interface every engine library exports (plan 21 / plan 23): a plain C interface, so a library
 * built by another compiler (MSVC and the CUDA toolkit for enp-engine-cuda.dll) loads into the app.
 *
 * The app never loads an engine library itself: `EasyNotepad --local-engine <library>` does, in a
 * helper process, so a fault in the library or its driver ends the helper, not the editor, and
 * the app goes on with its own CPU engine.
 *
 * Requests and results are JSON (src/engine/EngineJson.cpp). Memory never crosses: every string
 * returned is the engine's, valid until its next call. */
#ifndef ENP_ENGINE_H
#define ENP_ENGINE_H

#define ENP_ENGINE_ABI 1

#if defined(_WIN32)
#define ENP_ENGINE_EXPORT __declspec(dllexport)
#else
#define ENP_ENGINE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct enp_engine enp_engine;

/* While a request runs: kind "stage" (what it is doing, in words) or "text" (a piece of the answer). */
typedef void (*enp_engine_event_fn)(void* user, const char* kind, const char* text);
/* Asked often while a request runs: non-zero stops it at the next token. */
typedef int (*enp_engine_cancelled_fn)(void* user);

typedef int (*enp_engine_abi_fn)(void);
typedef const char* (*enp_engine_describe_fn)(void);
typedef enp_engine* (*enp_engine_create_fn)(void);
typedef const char* (*enp_engine_request_fn)(enp_engine* engine, const char* request_json, enp_engine_event_fn event,
                                             enp_engine_cancelled_fn cancelled, void* user);
typedef void (*enp_engine_unload_fn)(enp_engine* engine);
typedef void (*enp_engine_destroy_fn)(enp_engine* engine);

/* == ENP_ENGINE_ABI of the library's build; anything else is not loaded. */
ENP_ENGINE_EXPORT int enp_engine_abi(void);
/* {"name":"CUDA","version":"llama.cpp b12030 (...)","devices":[{"name","description","kind",
 *  "memoryTotal","memoryFree"}],"error":"..."}: the devices it can use; none means it cannot run here. */
ENP_ENGINE_EXPORT const char* enp_engine_describe(void);
ENP_ENGINE_EXPORT enp_engine* enp_engine_create(void);
/* {"op":"chat",...} or {"op":"run",...}; returns the result's JSON. The model stays loaded between chats. */
ENP_ENGINE_EXPORT const char* enp_engine_request(enp_engine* engine, const char* request_json, enp_engine_event_fn event,
                                                 enp_engine_cancelled_fn cancelled, void* user);
/* Frees the model (its memory back to the system); the next chat loads it again. */
ENP_ENGINE_EXPORT void enp_engine_unload(enp_engine* engine);
ENP_ENGINE_EXPORT void enp_engine_destroy(enp_engine* engine);

#ifdef __cplusplus
}
#endif

#endif
