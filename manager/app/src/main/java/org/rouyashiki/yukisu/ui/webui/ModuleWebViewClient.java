package org.rouyashiki.yukisu.ui.webui;

import android.content.Context;
import android.graphics.Bitmap;
import android.net.Uri;
import android.view.ViewGroup;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.webkit.WebViewAssetLoader;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.function.Consumer;

final class ModuleWebViewClient extends WebViewClient {
    private final Context context;
    private final WebViewAssetLoader assetLoader;
    private final Consumer<WebView> onRendererGone;
    private final Consumer<WebView> onPageChanged;

    ModuleWebViewClient(Context context, WebViewAssetLoader assetLoader,
            Consumer<WebView> onRendererGone, Consumer<WebView> onPageChanged) {
        this.context = context;
        this.assetLoader = assetLoader;
        this.onRendererGone = onRendererGone;
        this.onPageChanged = onPageChanged;
    }

    @Override
    public void doUpdateVisitedHistory(WebView view, String url, boolean isReload) {
        onPageChanged.accept(view);
        super.doUpdateVisitedHistory(view, url, isReload);
    }

    @Override
    public void onPageFinished(WebView view, String url) {
        onPageChanged.accept(view);
        super.onPageFinished(view, url);
    }

    @Override
    public boolean onRenderProcessGone(@NonNull WebView view,
            @NonNull RenderProcessGoneDetail detail) {
        if (view.getParent() instanceof ViewGroup parent) {
            parent.removeView(view);
        }
        view.destroy();
        onRendererGone.accept(view);
        return true;
    }

    @Nullable
    @Override
    public WebResourceResponse shouldInterceptRequest(@NonNull WebView view,
            @NonNull WebResourceRequest request) {
        Uri url = request.getUrl();
        if ("ksu".equalsIgnoreCase(url.getScheme()) && "icon".equalsIgnoreCase(url.getHost())) {
            String path = url.getPath();
            if (path != null && path.length() > 1) {
                Bitmap icon = AppIconUtil.INSTANCE.loadAppIconSync(context, path.substring(1), 512);
                if (icon != null) {
                    ByteArrayOutputStream stream = new ByteArrayOutputStream();
                    icon.compress(Bitmap.CompressFormat.PNG, 100, stream);
                    return new WebResourceResponse("image/png", null, 200, "OK",
                            Map.of("Access-Control-Allow-Origin", "*"),
                            new ByteArrayInputStream(stream.toByteArray()));
                }
                return new WebResourceResponse("text/plain", "utf-8", 404, "Not Found",
                        Map.of("Access-Control-Allow-Origin", "*"),
                        new ByteArrayInputStream("No such package".getBytes(StandardCharsets.UTF_8)));
            }
        }
        return assetLoader.shouldInterceptRequest(url);
    }
}
