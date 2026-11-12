Add-Type -ReferencedAssemblies System.Drawing @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public class HashTest {
    delegate bool EnumProc(IntPtr h, IntPtr l);

    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
    static extern IntPtr SendMessageText(IntPtr h, uint m, IntPtr w, string s);
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool repaint);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);

    // TCITEMW（x64）：mask/dwState/dwStateMask 各 4 字节，pszText 8 字节对齐到 16
    [StructLayout(LayoutKind.Sequential)]
    struct TCITEM {
        public uint mask;
        public uint dwState;
        public uint dwStateMask;
        public IntPtr pszText;
        public int cchTextMax;
        public int iImage;
        public IntPtr lParam;
    }
    [DllImport("user32.dll", EntryPoint = "SendMessageW")]
    static extern IntPtr SendMessageTcItem(IntPtr h, uint m, IntPtr w, ref TCITEM l);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    // 属性窗口标题以文件名开头
    public static IntPtr FindProps(string prefix) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            var t = new StringBuilder(512); GetWindowText(h, t, 512);
            if (t.ToString().StartsWith(prefix)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    static IntPtr FindTab(IntPtr win) {
        IntPtr tab = IntPtr.Zero;
        EnumChildWindows(win, (h, l) => {
            var c = new StringBuilder(256); GetClassName(h, c, 256);
            if (c.ToString() == "SysTabControl32") { tab = h; return false; }
            return true;
        }, IntPtr.Zero);
        return tab;
    }

    // 逐个切换标签页，优先按「哈希 / Hash」标题定位（比控件 ID 可靠：
    // 第三方属性页也可能撞 ID），找不到标题时退回 IDC_COPYALL(1008) 识别。
    public static int SelectLG_Hash(IntPtr win) {
        IntPtr tab = FindTab(win);
        if (tab == IntPtr.Zero) return -1;
        IntPtr sheet = GetParent(tab);
        if (sheet == IntPtr.Zero) sheet = tab;
        int count = (int)SendMessage(tab, 0x1304, IntPtr.Zero, IntPtr.Zero);
        // 第一轮：按标题
        for (int i = 0; i < count; i++) {
            string t = TabText(win, i);
            if (t == "哈希" || t == "Hash") { SelectPage(win, i); return i; }
        }
        // 第二轮：退回控件 ID 识别
        for (int i = 0; i < count; i++) {
            SendMessage(sheet, 0x0465, (IntPtr)i, IntPtr.Zero);
            bool found = false;
            EnumChildWindows(win, (h, l) => {
                if (GetDlgCtrlID(h) == 1008) { found = true; return false; }
                return true;
            }, IntPtr.Zero);
            if (found) return i;
        }
        return -1;
    }

    // TCM_GETITEMW 读第 index 个标签的文字
    public static string TabText(IntPtr win, int index) {
        IntPtr tab = FindTab(win);
        if (tab == IntPtr.Zero) return "";
        TCITEM it = new TCITEM();
        it.mask = 0x1;  // TCIF_TEXT
        it.pszText = Marshal.StringToHGlobalUni(new string('\0', 64));
        it.cchTextMax = 64;
        IntPtr ok = SendMessageTcItem(tab, 0x133C, (IntPtr)index, ref it);
        string s = ok == IntPtr.Zero ? "" : Marshal.PtrToStringUni(it.pszText);
        Marshal.FreeHGlobal(it.pszText);
        return s ?? "";
    }

    // 切到指定标签页（PSM_SETCURSEL），用于验证来回切页不会重复触发计算
    public static void SelectPage(IntPtr win, int index) {
        IntPtr tab = FindTab(win);
        if (tab == IntPtr.Zero) return;
        IntPtr sheet = GetParent(tab);
        if (sheet == IntPtr.Zero) sheet = tab;
        SendMessage(sheet, 0x0465, (IntPtr)index, IntPtr.Zero);
    }

    public static int PageCount(IntPtr win) {
        IntPtr tab = FindTab(win);
        if (tab == IntPtr.Zero) return 0;
        return (int)SendMessage(tab, 0x1304, IntPtr.Zero, IntPtr.Zero);
    }

    public static Dictionary<int, IntPtr> Children(IntPtr win) {
        var d = new Dictionary<int, IntPtr>();
        EnumChildWindows(win, (h, l) => { d[GetDlgCtrlID(h)] = h; return true; }, IntPtr.Zero);
        return d;
    }

    public static void SetText(IntPtr h, string s) { SendMessageText(h, 0x000C, IntPtr.Zero, s); }

    public static string GetText(IntPtr h) {
        int len = (int)SendMessage(h, 0x000E, IntPtr.Zero, IntPtr.Zero);
        var sb = new StringBuilder(Math.Max(len + 2, 16));
        GetWindowText(h, sb, sb.Capacity);
        return sb.ToString();
    }

    public static bool Enabled(IntPtr h) { return IsWindowEnabled(h); }
    public static bool Visible(IntPtr h) { return h != IntPtr.Zero && IsWindowVisible(h); }
    public static void Close(IntPtr h) { SendMessage(h, 0x0010, IntPtr.Zero, IntPtr.Zero); }
    // 激活窗口并把输入焦点给它（SendKeys 前置步骤，避免按键落进别的窗口）
    public static void Focus(IntPtr h) {
        SetForegroundWindow(h);
        BringWindowToTop(h);
    }

    // 设置复选框勾选状态（BM_SETCHECK）。跨进程 Post WM_COMMAND 不会让控件自切，
    // 测试须先 SetCheck 再 SendCommand，模拟真实点击后的通知。
    public static void SetCheck(IntPtr h, bool on) {
        SendMessage(h, 0x00F1, (IntPtr)(on ? 1 : 0), IntPtr.Zero);
    }

    // 直接读 ListView 某行某列的文字，绕开剪贴板
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
    static extern IntPtr SendMsgItem(IntPtr h, uint m, IntPtr w, ref LVITEMW l);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct LVITEMW {
        public uint mask;
        public int iItem;
        public int iSubItem;
        public uint state;
        public uint stateMask;
        public IntPtr pszText;
        public int cchTextMax;
        public int iImage;
        public IntPtr lParam;
        public int iIndent;
        public int iGroupId;
        public uint cColumns;
        public IntPtr puColumns;
    }

    // LVM_GETITEMTEXTW = LVM_FIRST + 115
    public static string ListItemText(IntPtr list, int item, int sub) {
        const int cap = 512;
        IntPtr buf = Marshal.AllocHGlobal(cap * 2);
        try {
            var lv = new LVITEMW();
            lv.iSubItem = sub;
            lv.pszText = buf;
            lv.cchTextMax = cap;
            int len = (int)SendMsgItem(list, 0x1073, (IntPtr)item, ref lv);
            if (len <= 0) return string.Empty;
            return Marshal.PtrToStringUni(buf, len);
        } finally {
            Marshal.FreeHGlobal(buf);
        }
    }

    // 行数（LVM_GETITEMCOUNT）——列表行随勾选算法动态增减
    public static int ListItemCount(IntPtr list) {
        return (int)SendMessage(list, 0x1004, IntPtr.Zero, IntPtr.Zero);
    }

    // 把窗口挪到左上角并强制置前
    public static void ParkWindow(IntPtr h) {
        RECT r;
        if (!GetWindowRect(h, out r)) return;
        int w = r.Right - r.Left, hh = r.Bottom - r.Top;
        if (w < 200 || hh < 100) return;

        ShowWindow(h, 9);   // SW_RESTORE
        MoveWindow(h, 10, 10, w, hh, true);
        BringWindowToTop(h);

        System.Threading.Thread.Sleep(300);
    }

    // 用 PrintWindow 抓取窗口内容，不受遮挡影响
    public static bool CaptureWindowPrint(IntPtr h, string path) {
        RECT r;
        if (!GetWindowRect(h, out r)) return false;
        int w = r.Right - r.Left, hh = r.Bottom - r.Top;
        if (w <= 0 || hh <= 0) return false;
        using (var bmp = new Bitmap(w, hh))
        using (var g = Graphics.FromImage(bmp)) {
            IntPtr hdc = g.GetHdc();
            bool ok = PrintWindow(h, hdc, 2); // PW_RENDERFULLCONTENT
            g.ReleaseHdc(hdc);
            bmp.Save(path, ImageFormat.Png);
            return ok;
        }
    }

    // 按窗口矩形直接截屏（要求窗口可见且在前）
    public static bool CaptureWindow(IntPtr h, string path) {
        RECT r;
        if (!GetWindowRect(h, out r)) return false;
        int w = r.Right - r.Left, hh = r.Bottom - r.Top;
        if (w <= 0 || hh <= 0) return false;
        using (var bmp = new Bitmap(w, hh))
        using (var g = Graphics.FromImage(bmp)) {
            g.CopyFromScreen(r.Left, r.Top, 0, 0, new Size(w, hh));
            bmp.Save(path, ImageFormat.Png);
        }
        return true;
    }

    // 向对话框发 WM_COMMAND，等价于点按按钮
    public static void SendCommand(IntPtr dlg, int id) {
        PostMessage(dlg, 0x0111, (IntPtr)(id & 0xFFFF), IntPtr.Zero);
    }

    // 按标题精确查找顶层对话框（设置对话框等）。
    // 必须同时校验类名为 #32770：桌面上可能同时存在 Windows 自己的「设置」窗口。
    public static IntPtr FindDlg(string title) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            var c = new StringBuilder(64); GetClassName(h, c, 64);
            if (c.ToString() != "#32770") return true;
            var t = new StringBuilder(512); GetWindowText(h, t, 512);
            if (t.ToString() == title) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    static IntPtr FindByClass(IntPtr dlg, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(dlg, (h, l) => {
            var c = new StringBuilder(64); GetClassName(h, c, 64);
            if (c.ToString() == cls) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    // 判断对话框里是否有文字以指定前缀开头的按钮（"保存(&S)" 也算）
    static bool HasButton(IntPtr dlg, string prefix) {
        bool found = false;
        EnumChildWindows(dlg, (h, l) => {
            var c = new StringBuilder(64); GetClassName(h, c, 64);
            if (c.ToString() != "Button") return true;
            var t = new StringBuilder(128); GetWindowText(h, t, 128);
            if (t.ToString().StartsWith(prefix)) { found = true; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    static IntPtr FindById(IntPtr dlg, int id) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(dlg, (h, l) => {
            if (GetDlgCtrlID(h) == id) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    // 现代保存对话框（IFileSaveDialog / 旧式另存为）的文件名 Edit 控件 id 为 1001
    public static void SetDialogFileName(IntPtr dlg, string path) {
        IntPtr edit = FindById(dlg, 1001);
        if (edit == IntPtr.Zero) {
            IntPtr combo = FindByClass(dlg, "ComboBox");
            if (combo != IntPtr.Zero) edit = FindByClass(combo, "Edit");
        }
        if (edit == IntPtr.Zero) edit = FindByClass(dlg, "Edit");
        if (edit == IntPtr.Zero) throw new Exception("file name edit not found");
        SetText(edit, path);
    }

    public static void AcceptDialog(IntPtr dlg) {
        PostMessage(dlg, 0x0111, (IntPtr)1, IntPtr.Zero);
    }

    // 保存对话框：标题含默认文件名，或为“另存为”/“Save As”
    public static IntPtr FindSaveDialog(string fileName) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
            var c = new StringBuilder(64); GetClassName(h, c, 64);
            if (c.ToString() != "#32770") return true;
            var t = new StringBuilder(512); GetWindowText(h, t, 512);
            string title = t.ToString();

            // 必须同时满足：
            //   1) 标题像保存对话框（含默认文件名，或“另存为/Save As”）
            //   2) 存在 id=1001 的文件名编辑框
            //   3) 存在"保存/Save"按钮 —— 属性页里也有 id=1001 的标签，
            //      只靠标题或控件 ID 会误命中属性窗口。
            bool titleMatch = title.IndexOf(fileName, StringComparison.OrdinalIgnoreCase) >= 0 ||
                              title.StartsWith("另存为") || title.StartsWith("Save As");
            if (!titleMatch) return true;

            IntPtr edit = FindById(h, 1001);
            if (edit == IntPtr.Zero) return true;
            if (!HasButton(h, "保存") && !HasButton(h, "Save")) return true;

            found = h;
            return false;
        }, IntPtr.Zero);
        return found;
    }

    // 把所有 #32770 窗口的 pid:标题 列出来，便于排查
    public static string[] DlgWins() {
        var list = new List<string>();
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
            var c = new StringBuilder(64); GetClassName(h, c, 64);
            if (c.ToString() == "#32770") {
                var t = new StringBuilder(256); GetWindowText(h, t, 256);
                list.Add(t.ToString());
            }
            return true;
        }, IntPtr.Zero);
        return list.ToArray();
    }

    public static void CancelDialog() {
        IntPtr fg = GetForegroundWindow();
        if (fg != IntPtr.Zero) PostMessage(fg, 0x0100, (IntPtr)0x1B, IntPtr.Zero);
        if (fg != IntPtr.Zero) PostMessage(fg, 0x0101, (IntPtr)0x1B, IntPtr.Zero);
    }
}
'@

function Get-ClipboardRetry {
    param([int]$Attempts = 20)
    for ($i = 0; $i -lt $Attempts; $i++) {
        try { return (Get-Clipboard -Raw) } catch { Start-Sleep -Milliseconds 100 }
    }
    throw 'clipboard unavailable'
}
