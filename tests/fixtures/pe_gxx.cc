// SPDX-License-Identifier: GPL-2.0-only
/*
 * C++ exceptions as mingw-w64's g++ compiles them for Win64, with libgcc and
 * libstdc++ linked in as LAV Filters' DLLs are: __gxx_personality_seh0 and
 * libgcc's unwinder, which walk the stack with RtlLookupFunctionEntry and
 * RtlVirtualUnwind and unwind with RtlUnwindEx. Built only where a mingw-w64
 * g++ is found.
 */
#include <windows.h>
#include <stdexcept>
#include <string>
static int dtors;
struct G { ~G() { dtors++; } };
struct E { int v; };
static int thrower(int v) { G g; if (v > 0) throw E{v}; if (v < 0) throw std::runtime_error("neg"); return 0; }
static int middle(int v) { G a; G b; return thrower(v) + 1; }
extern "C" __declspec(dllexport) int gxx_int(int v) { try { return middle(v); } catch (E &e) { return e.v; } }
extern "C" __declspec(dllexport) int gxx_std(int v) { try { return middle(v); } catch (std::exception &e) { return (int)std::string(e.what()).size(); } }
extern "C" __declspec(dllexport) int gxx_all(int v) { try { return middle(v); } catch (...) { return 77; } }
extern "C" __declspec(dllexport) int gxx_nested(int v) { try { try { return middle(v); } catch (E &e) { throw; } } catch (E &e) { return e.v * 2; } }
extern "C" __declspec(dllexport) int gxx_dtors(void) { return dtors; }
extern "C" __declspec(dllexport) int gxx_dyn(void) { struct A { virtual ~A(){} }; struct B : A {}; A *p = new B; int r = dynamic_cast<B*>(p) != nullptr; delete p; return r; }
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
