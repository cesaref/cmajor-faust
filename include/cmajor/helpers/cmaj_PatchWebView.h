//
//     ,ad888ba,                              88
//    d8"'    "8b
//   d8            88,dba,,adba,   ,aPP8A.A8  88     The Cmajor Toolkit
//   Y8,           88    88    88  88     88  88
//    Y8a.   .a8P  88    88    88  88,   ,88  88     (C)2024 Cmajor Software Ltd
//     '"Y888Y"'   88    88    88  '"8bbP"Y8  88     https://cmajor.dev
//                                           ,88
//                                        888P"
//
//  The Cmajor project is subject to commercial or open-source licensing.
//  You may use it under the terms of the GPLv3 (see www.gnu.org/licenses), or
//  visit https://cmajor.dev to learn about our commercial licence options.
//
//  CMAJOR IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
//  EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
//  DISCLAIMED.

#pragma once

#include "cmaj_Patch.h"
#include "../../choc/gui/choc_WebView.h"

#include <memory>

namespace cmaj
{

//==============================================================================
/// A HTML patch GUI implementation.
struct PatchWebView  : public PatchView
{
    /// Map a file extension (".html", ".js") to a MIME type (i.e. "text/html", "text/javascript").
    /// A default implementation is provided, but it is non-exhaustive. If a custom mapping function is given,
    /// it will be called first, falling back to the default implementation if an empty result is returned.
    using MimeTypeMappingFn = std::function<std::string(std::string_view extension)>;

    static std::unique_ptr<PatchWebView> create (Patch&, const PatchManifest::View&, MimeTypeMappingFn = {});
    ~PatchWebView() override;

    choc::ui::WebView& getWebView();

    void sendMessage (const choc::value::ValueView&) override;

    void reload();

private:
    struct Impl;

    PatchWebView (std::unique_ptr<Impl>, const PatchManifest::View&);

    std::unique_ptr<Impl> pimpl;
};


//==============================================================================
//        _        _           _  _
//     __| |  ___ | |_   __ _ (_)| | ___
//    / _` | / _ \| __| / _` || || |/ __|
//   | (_| ||  __/| |_ | (_| || || |\__ \ _  _  _
//    \__,_| \___| \__| \__,_||_||_||___/(_)(_)(_)
//
//   Code beyond this point is implementation detail...
//
//==============================================================================

struct PatchWebView::Impl
{
    Impl (Patch& p, MimeTypeMappingFn toMimeTypeFn)
        : patch (p), toMimeTypeCustomImpl (std::move (toMimeTypeFn))
    {
        createBindings();
    }

    void sendMessage (const choc::value::ValueView& msg)
    {
        if (! webview.evaluateJavascript ("window.cmaj_deliverMessageFromServer?.(" + choc::json::toString (msg, true) + ");"))
            CMAJ_ASSERT_FALSE;
    }

    void createBindings()
    {
        bool boundOK = webview.bind ("cmaj_sendMessageToServer", [this] (const choc::value::ValueView& args) -> choc::value::Value
        {
            try
            {
                if (args.isArray() && args.size() != 0)
                    patch.handleClientMessage (*ownerView, args[0]);
            }
            catch (const std::exception& e)
            {
                std::cout << "Error processing message from client: " << e.what() << std::endl;
            }

            return {};
        });

        (void) boundOK;
        CMAJ_ASSERT (boundOK);
    }

    PatchWebView* ownerView = nullptr;
    Patch& patch;
    MimeTypeMappingFn toMimeTypeCustomImpl;

   #if CMAJ_ENABLE_WEBVIEW_DEV_TOOLS
    static constexpr bool allowWebviewDevMode = true;
   #else
    static constexpr bool allowWebviewDevMode = false;
   #endif

    choc::ui::WebView webview { { allowWebviewDevMode, true, {}, [this] (const auto& path) { return onRequest (path); } } };

    using ResourcePath = choc::ui::WebView::Options::Path;
    using OptionalResource = std::optional<choc::ui::WebView::Options::Resource>;
    OptionalResource onRequest (const ResourcePath&);

    static std::string toMimeTypeDefaultImpl (std::string_view extension);
};

inline std::unique_ptr<PatchWebView> PatchWebView::create (Patch& p, const PatchManifest::View& view, MimeTypeMappingFn toMimeType)
{
    auto impl = std::make_unique <PatchWebView::Impl> (p, std::move (toMimeType));
    return std::unique_ptr<PatchWebView> (new PatchWebView (std::move (impl), view));
}

inline PatchWebView::PatchWebView (std::unique_ptr<Impl> impl, const PatchManifest::View& view)
    : PatchView (impl->patch, view), pimpl (std::move (impl))
{
    pimpl->ownerView = this;
}

inline PatchWebView::~PatchWebView() = default;

inline choc::ui::WebView& PatchWebView::getWebView()
{
    return pimpl->webview;
}

inline void PatchWebView::sendMessage (const choc::value::ValueView& msg)
{
    return pimpl->sendMessage (msg);
}

inline void PatchWebView::reload()
{
    getWebView().evaluateJavascript ("document.location.reload()");
}

static constexpr auto cmajor_patch_gui_html = R"(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <title>Cmajor Patch Controls</title>
</head>

<style>
  * { box-sizing: border-box; padding: 0; margin: 0; border: 0; }
  html { background: black; overflow: hidden; }
  body { display: block; position: absolute; width: 100%; height: 100%; }
  #cmaj-outer-container { display: block; position: relative; width: 100%; height: 100%; overflow: auto; }
  #cmaj-inner-container { display: block; position: relative; width: 100%; height: 100%; overflow: visible; transform-origin: 0% 0%; }
</style>

<body>
  <div id="cmaj-outer-container">
    <div id="cmaj-inner-container"></div>
  </div>
</body>

<script type="module">

import { PatchConnection } from "../cmaj_api/cmaj-patch-connection.js"
import { createPatchView, scalePatchViewToFit } from "./cmaj_api/cmaj-patch-view.js"

//==============================================================================
const patchManifest = $MANIFEST$;

const viewInfo = $VIEW_TO_USE$;

//==============================================================================
class EmbeddedPatchConnection  extends PatchConnection
{
    constructor()
    {
        super();
        this.manifest = patchManifest;
        window.cmaj_deliverMessageFromServer = msg => this.deliverMessageFromServer (msg);
    }

    getResourceAddress (path)
    {
        return path.startsWith ("/") ? path : ("/" + path);
    }

    sendMessageToServer (message)
    {
        window.cmaj_sendMessageToServer (message);
    }
}

//==============================================================================
const outer = document.getElementById ("cmaj-outer-container");
const inner = document.getElementById ("cmaj-inner-container");

const patchConnection = new EmbeddedPatchConnection();
createPatchView (patchConnection, viewInfo).then ((currentView) =>
{
    inner.appendChild (currentView);

    const resizeObserver = new ResizeObserver (() => scalePatchViewToFit (currentView, inner, outer));
    resizeObserver.observe (document.body);

    scalePatchViewToFit (currentView, inner, outer);
});

</script>
</html>
)";

inline PatchWebView::Impl::OptionalResource PatchWebView::Impl::onRequest (const ResourcePath& path)
{
    const auto toResource = [] (std::string_view content, const auto& mimeType) -> choc::ui::WebView::Options::Resource
    {
        return
        {
            std::vector<uint8_t> (reinterpret_cast<const uint8_t*> (content.data()),
                                  reinterpret_cast<const uint8_t*> (content.data()) + content.size()),
            mimeType
        };
    };

    const auto toMimeType = [this] (const auto& extension)
    {
        const auto mimeType = toMimeTypeCustomImpl ? toMimeTypeCustomImpl (extension) : std::string {};
        return mimeType.empty() ? toMimeTypeDefaultImpl (extension) : mimeType;
    };

    auto relativePath = std::filesystem::path (path).relative_path();
    bool wantsRootHTMLPage = relativePath.empty();

    if (wantsRootHTMLPage)
    {
        choc::value::Value manifestObject;
        cmaj::PatchManifest::View viewToUse;

        if (auto manifest = patch.getManifest())
        {
            manifestObject = manifest->manifest;

            if (auto v = manifest->findDefaultView())
                viewToUse = *v;
        }

        return toResource (choc::text::replace (cmajor_patch_gui_html,
                                                "$MANIFEST$", choc::json::toString (manifestObject, true),
                                                "$VIEW_TO_USE$", choc::json::toString (viewToUse.view, true)),
                           toMimeType (".html"));
    }

    if (auto content = readJavascriptResource (path, patch.getManifest()))
        if (! content->empty())
            return toResource (*content, toMimeType (relativePath.extension().string()));

    return {};
}

inline std::string PatchWebView::Impl::toMimeTypeDefaultImpl (std::string_view extension)
{
    if (extension == ".css")  return "text/css";
    if (extension == ".html") return "text/html";
    if (extension == ".js")   return "text/javascript";
    if (extension == ".mjs")  return "text/javascript";
    if (extension == ".svg")  return "image/svg+xml";
    if (extension == ".wasm") return "application/wasm";

    return "application/octet-stream";
}

} // namespace cmaj
