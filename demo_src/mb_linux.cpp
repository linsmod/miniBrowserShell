
//
// 部分api文档地址：https://miniblink.net/views/doc/api-doc-vip.html
// 目前已导出的api:
// mbInit
// mbUninit
// mbCreateInitSettings
// mbSetInitSettings
// mbCreateWebWindow
// mbOnLoadUrlBegin
// mbOnTitleChanged
// mbOnURLChanged
// mbOnCreateView
// mbOnConsole
// mbLoadURL
// mbShowWindow
// mbRunMessageLoop
// mbExitMessageLoop
// mbPostToUiThread
// mbGetString
// mbGetStringLen
// mbDeleteString
// mbCreateString
// mbNetSetData
// mbNetSetMIMEType
// mbNetSetHTTPHeaderFieldUtf8
// mbIsMainFrame
// mbReload
// mbGoForward
// mbGoBack
// mbResize
// mbGetSize
// mbWake
// mbDestroyWebView
// mbNetCancelRequest
// mbPopupDialogAndDownload
// mbOnDownloadInBlinkThread
// mbOnPrinting
// mbSetNavigationToNewWindowEnable
// mbOnClose
// mbQueryState
// mbNetChangeRequestUrl
// mbRunJs
// mbWebFrameGetMainFrame
// mbResponseQuery
// mbOnJsQuery
// mbCreateWebView
// mbOnPaintUpdated
// mbGetHostHWND
// mbMoveToCenter
// mbSetHandle
// mbSetFocus
// mbKillFocus
// mbSetDebugConfig
// mbJsToBoolean
// mbJsToDouble
// mbJsToString
// mbGetLockedViewDC
// mbUnlockViewDC
// mbOnLoadingFinish
// mbOnLoadUrlEnd
// mbOnDocumentReady
// mbOnDownload
// mbOnAlertBox
// mbOnConfirmBox
// mbOnPromptBox
// mbOnImageBufferToDataURL
// mbOnNavigation
// mbOnDidCreateScriptContext
// mbSetCspCheckEnable
// mbSetCookieEnabled
// mbFireKeyDownEvent
// mbFireKeyPressEvent
// mbFireKeyUpEvent
// mbFireMouseWheelEvent
// mbFireWindowsMessage
// mbFireMouseEvent
// mbSetResourceGc
// mbSetUserKeyValue
// mbGetUserKeyValue
// mbGetSource
// mbGetSize

#if !defined(_WIN32)

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <utility>
#include <stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <signal.h>
#include <dlfcn.h>
#include <string>
#include "../../mbvip/core/mb.h"

#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>

const char* kHtmlTestStr =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"    <meta charset=\"UTF-8\">\n"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale = 1.0\">\n"
"    <title>test</title>\n"
"    <style>\n"
"        .red-rectangle {\n"
"            width: 200px;\n"
"            height: 50px;\n"
"            background-color: red;\n"
"            -webkit-app-region: drag;\n"
"        }\n"
"    </style>\n"
"</head>\n"
"<body>\n"
"    <div class=\"red-rectangle\"></div>\n"
"</body>\n"
"</html>\n"
;

extern "C" unsigned long _stack_chk_guard;

mbWebView MB_CALL_TYPE handleCreateView(mbWebView webView, void* param, mbNavigationType navigationType, const utf8* url, const mbWindowFeatures* windowFeatures)
{
    mbWebView mbView = mbCreateWebWindow(MB_WINDOW_TYPE_POPUP, NULL, 110, 60, 600, 700);
    ::mbOnCreateView(mbView, handleCreateView, nullptr);
    ::mbSetNavigationToNewWindowEnable(mbView, true);
    ::mbSetCspCheckEnable(mbView, true);

    ::mbShowWindow(mbView, TRUE);
    return mbView;
}

static BOOL MB_CALL_TYPE handleLoadUrlBegin(mbWebView webView, void* param, const char* url, void* job)
{
    printf("handleLoadUrlBegin: %s\n", url);
     
    //     std::string urlStr("handleLoadUrlBegin:");
    //     urlStr += url;
    //     urlStr += "\n";
    //     printf(urlStr.c_str());
    //
    //     if (hookUrl(job, url, "universal-report.min.js", L"G:\\test\\web_test\\51iwifi\\universal-report.min.js", "text/html"))
    //         return true;
    // 
    //     if (hookUrl(job, url, "_app-f54e3843f15fa10c7198.js", L"D:\\test\\web\\cc_163\\_app-f54e3843f15fa10c7198.js", "text/javascript"))
    //         return true;
    // 
    //     if (hookUrl(job, url, "act/webcc/link-pk-game/v1.9.1/index.js", L"D:\\test\\web\\cc_163\\webcc_191_index.js", "text/javascript"))
    //         return true;
    // 
    //     if (hookUrl(job, url, "act/webcc/performance-reporter/v1.2.0/index.js", L"D:\\test\\web\\cc_163\\performance-reporter.js", "text/javascript"))
    //         return true;
    // 
    //     if (0 != strstr(url, "mini_original.js?v")) {
    //         mbNetChangeRequestUrl(job, "http://192.168.83.1:8080/mini_original.js");
    //     }
    return TRUE;
}

void MB_CALL_TYPE onJsQueryCallback(mbWebView webView, void* param, mbJsExecState es, int64_t queryId, int customMsg, const utf8* request)
{
    printf("onJsQueryCallback");
    mbResponseQuery(webView, queryId, customMsg, "I am response");
}

int main(int argc, char** argv)
{
    mbInit(nullptr);

    //mbWebView mbView = mbCreateWebWindow(MB_WINDOW_TYPE_POPUP, NULL, 100, 50, 600, 700); // 创建一个普通窗口
    mbWebView mbView = ::mbCreateWebCustomWindow(NULL, WS_POPUP, 0, 100, 50, 600, 700); // 创建了一个无标题栏无边框的窗口

    ::mbSetWindowTitle(mbView, "miniblink108");
    ::mbOnLoadUrlBegin(mbView, handleLoadUrlBegin, nullptr);
    ::mbOnCreateView(mbView, handleCreateView, nullptr);
    ::mbOnJsQuery(mbView, onJsQueryCallback, nullptr);
    ::mbSetDebugConfig(mbView, "ncHittestPaddingWidth", "2"); // 设置边框边缘多长为可拉伸

    const char* url =
        //"view-source:https://www.baidu.com"
        "http://192.168.0.107:8091/linux_mb/index.html";
        //"baidu.com";

    ::mbLoadURL(mbView, url); // 通过url加载网页
    //::mbLoadHtmlWithBaseUrl(mbView, kHtmlTestStr, "file:///test.html"); // 通过字符串加载html

    ::mbShowWindow(mbView, TRUE);

    void* gtkWin = mbGetPlatformWindowHandle(mbView); // 获取gtk的GtkWidget*，但不一定能成功，需要等待窗口创建完毕才行
    printf("gtkWin::::::: %p\n", gtkWin);

    ::mbRunMessageLoop(); // 需要退出可调用mbExitMessageLoop

    return 0;
}

#endif