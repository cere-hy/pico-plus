import dev.readpico.push.PushProto;

/** 三端对表 · Java 端 runner（复用 PushProto.bookIdFromPath）。
 *  与 C `tri_c.c` / Python `tri_py.py` 同一套 9 个文件名（顺序一致）。 */
public class TriVec {
    public static void main(String[] args) {
        String[] samples = {
            "/books/Hold On to Your Kids.epub",
            "/books/战争与和平.txt",
            "/books/The.Great.Gatsby.epub",
            "/books/note.tar.gz",
            "/books/README",
            "/books/UPPER.EPUB",
            "/books/upper.epub",
            "/books/.hidden",
            "/sd/books/中文 书名.epub",
        };
        for (String s : samples) {
            System.out.printf("0x%08x%n", PushProto.bookIdFromPath(s));
        }
    }
}
