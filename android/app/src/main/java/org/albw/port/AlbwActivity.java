package org.albw.port;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;

public class AlbwActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    /* ---- HD texture pack import (README: HD textures; native side platform/hd_ui.c)
     * Android lets an app read a folder of the user's choice only through the system folder picker
     * (Storage Access Framework), which hands back a content URI, not a path. Import walks the
     * chosen folder (any depth: the pack's top folder works), copies every tex1_*.png and pack.json
     * into <app files>/textures/<folder name>/, where render/hdtex.c reads them with plain file I/O.
     * State for the native side, polled each frame: 0 idle, 1 picking, 2 copying (done / total),
     * 3 finished, -1 failed, -2 cancelled, -3 no textures in that folder. */
    private static final int REQ_TEXTURES = 0xA1B;
    public static volatile int sImportState;
    public static volatile int sImportDone, sImportTotal;

    public static void importTexturePack() {
        final SDLActivity a = mSingleton;
        if (a == null) { sImportState = -1; return; }
        sImportState = 1;
        a.runOnUiThread(() -> {
            try {
                a.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_TEXTURES);
            } catch (Exception e) {
                sImportState = -1;
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQ_TEXTURES) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            sImportState = -2;
            return;
        }
        final Uri tree = data.getData();
        new Thread(() -> copyPack(tree), "albw-import").start();
    }

    private void copyPack(Uri tree) {
        try {
            String rootId = DocumentsContract.getTreeDocumentId(tree);
            String name = displayName(tree, rootId);
            ArrayList<String[]> files = new ArrayList<>();   /* {document id, file name} */
            walk(tree, rootId, files, 0);
            if (files.isEmpty()) { sImportState = -3; return; }
            File dir = new File(getExternalFilesDir(null), "textures/" + safeName(name));
            if (!dir.isDirectory() && !dir.mkdirs()) { sImportState = -1; return; }
            sImportTotal = files.size();
            sImportDone = 0;
            sImportState = 2;
            byte[] buf = new byte[1 << 16];
            for (String[] f : files) {
                Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, f[0]);
                File out = new File(dir, f[1]);
                try (InputStream in = getContentResolver().openInputStream(doc);
                     OutputStream o = new FileOutputStream(out)) {
                    int n;
                    while (in != null && (n = in.read(buf)) > 0) o.write(buf, 0, n);
                }
                sImportDone++;
            }
            sImportState = 3;
        } catch (Exception e) {
            sImportState = -1;
        }
    }

    private void walk(Uri tree, String parentId, ArrayList<String[]> out, int depth) {
        if (depth > 10) return;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] cols = { DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                          DocumentsContract.Document.COLUMN_MIME_TYPE };
        try (Cursor c = getContentResolver().query(children, cols, null, null, null)) {
            if (c == null) return;
            while (c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) walk(tree, id, out, depth + 1);
                else if (name != null && ((name.startsWith("tex1_") && name.toLowerCase().endsWith(".png")) || name.equals("pack.json")))
                    out.add(new String[] { id, name });
            }
        }
    }

    private String displayName(Uri tree, String id) {
        try (Cursor c = getContentResolver().query(DocumentsContract.buildDocumentUriUsingTree(tree, id),
                new String[] { DocumentsContract.Document.COLUMN_DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) return c.getString(0);
        } catch (Exception e) {
            /* fall through */
        }
        return "pack";
    }

    private static String safeName(String s) {
        String r = s.replaceAll("[^A-Za-z0-9._ ()-]", "_").trim();
        return r.isEmpty() ? "pack" : r;
    }
}
