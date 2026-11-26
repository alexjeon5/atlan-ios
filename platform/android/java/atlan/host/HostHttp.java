// SPDX-License-Identifier: GPL-2.0-or-later
package atlan.host;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.MalformedURLException;
import java.net.URL;

// HTTPS requests of the guest's Java side, made with the platform's own TLS stack. Each request runs on its own
// thread and ends in exactly one nativeDone (outcome 0 = a response with `status`, 1 = transport error, 2 = bad URL).
final class HostHttp {
    private HostHttp() {}

    static void fetch(final long token, final String method, final String url, final String[] headers,
                      final byte[] body, final int timeoutMs) {
        Thread thread = new Thread(() -> run(token, method, url, headers, body, timeoutMs), "host-http");
        thread.setDaemon(true);
        thread.start();
    }

    private static void run(long token, String method, String url, String[] headers, byte[] body, int timeoutMs) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(timeoutMs);
            connection.setReadTimeout(timeoutMs);
            connection.setRequestMethod(method);
            connection.setUseCaches(false);
            for (int i = 0; i + 1 < headers.length; i += 2) connection.setRequestProperty(headers[i], headers[i + 1]);
            if (body != null) {
                connection.setDoOutput(true);
                connection.setFixedLengthStreamingMode(body.length);
                try (OutputStream out = connection.getOutputStream()) {
                    out.write(body);
                }
            }
            int status = connection.getResponseCode();
            InputStream in = status >= 400 ? connection.getErrorStream() : connection.getInputStream();
            nativeDone(token, 0, status, in == null ? new byte[0] : readAll(in), null);
        } catch (MalformedURLException e) {
            nativeDone(token, 2, 0, null, e.toString());
        } catch (Throwable e) {
            nativeDone(token, 1, 0, null, e.toString());
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    private static byte[] readAll(InputStream in) throws java.io.IOException {
        try (InputStream stream = in) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[16384];
            for (int n; (n = stream.read(buffer)) > 0; ) out.write(buffer, 0, n);
            return out.toByteArray();
        }
    }

    private static native void nativeDone(long token, int outcome, int status, byte[] body, String error);
}
