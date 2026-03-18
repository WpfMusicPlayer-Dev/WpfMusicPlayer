// pch.h: 这是预编译标头文件。
// 下方列出的文件仅编译一次，提高了将来生成的生成性能。
// 这还将影响 IntelliSense 性能，包括代码完成和许多代码浏览功能。
// 但是，如果此处列出的文件中的任何一个在生成之间有更新，它们全部都将被重新编译。
// 请勿在此处添加要频繁更新的文件，这将使得性能优势无效。

#ifndef PCH_H
#define PCH_H

#if defined(_MSC_VER) // if uses msvc...
#define _CRT_SECURE_NO_WARNINGS // NOLINT(*-reserved-identifier)
#pragma warning (disable: 4819) // avoid msvc utf-8 warning
#endif

#define _CRTDBG_MAP_ALLOC // NOLINT(*-reserved-identifier)
#ifdef _DEBUG
#define DBG_NEW new ( _NORMAL_BLOCK , __FILE__ , __LINE__ )
// Replace _NORMAL_BLOCK with _CLIENT_BLOCK if you want the
// allocations to be of _CLIENT_BLOCK type
#else
#define DBG_NEW new
#endif

// 添加要在此处预编译的标头

#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS      // 某些 CString 构造函数将是显式的 NOLINT(*-reserved-identifier)

// 关闭 MFC 的一些常见且经常可放心忽略的隐藏警告消息
#define _AFX_ALL_WARNINGS // NOLINT(*-reserved-identifier)

#include <afxwin.h>         // MFC 核心组件和标准组件
#include <afxext.h>         // MFC 扩展

#include <atltrace.h>       // For Debug
#include <atlcoll.h>        // ATL Header File

#include <afxdisp.h>        // MFC 自动化类

#ifndef _AFX_NO_OLE_SUPPORT
#include <afxdtctl.h>           // MFC 对 Internet Explorer 4 公共控件的支持
#endif
#ifndef _AFX_NO_AFXCMN_SUPPORT
#include <afxcmn.h>             // MFC 对 Windows 公共控件的支持
#endif // _AFX_NO_AFXCMN_SUPPORT

#include <afxdialogex.h>
#include <afxdlgs.h>
#include <afxinet.h>
#include <afxcontrolbars.h>     // MFC 支持功能区和控制条

#include <cstdlib>
#include <cassert>
#include <list>
#include <functional>
#include <future>
#include <string>
#include <stack>
#include <algorithm>
#include <deque>
#include <cstdio>
#include <cinttypes>

#include <avrt.h>
#include <comdef.h>
#include <xaudio2.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <shlwapi.h>
#include <functiondiscoverykeys_devpkey.h>

#if defined(__cplusplus)
extern "C" {
#endif
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/frame.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>

#if defined(__cplusplus)
}
#endif

#if !defined(FFMPEG_CRITICAL_ERROR)
#define FFMPEG_CRITICAL_ERROR(err_code) \
	do { \
		dialog_ffmpeg_critical_error(err_code, __FILE__, __LINE__); \
	} while(0)
#endif

#if !defined(tstring)
#if defined(UNICODE)
#define tstring wstring
#else
#define tstring string
#endif
#endif
#if !defined(WAY3RES)
#define WAY3RES(ord) \
((ord) == std::strong_ordering::less ? ThreeWayCompareResult::Less : \
(ord) == std::strong_ordering::greater ? ThreeWayCompareResult::Greater : \
ThreeWayCompareResult::Equal)
#endif
#endif //PCH_H
