// LLM Deck 미러링 앱 (Windows). 브라우저 대신 작은 창에 기기 화면을 띄우고, 눌러서 기기를 조작한다 (USB 나 Wi-Fi 로 연결했을 때).
// 도우미(mirror.py)가 이 PC 안에 띄운 http://127.0.0.1:4013/ 에서 화면을 받아 가고 누르기를 보낸다. 하는 일은 mirror.py 의 PAGE 와 같다.
// 이 창이 화면을 받아 가는 동안만 기기가 화면을 보낸다.
//
// install.ps1 이 Windows 에 처음부터 들어 있는 .NET Framework 4 의 csc.exe 로 컴파일한다 (따로 설치할 것이 없다).
// 그 csc 는 C# 5 까지만 알아서 $"..." 나 ?. 같은 새 문법은 쓰지 않는다.
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

class MirrorForm : Form
{
    const string Base = "http://127.0.0.1:4013/";
    const string Title = "LLM Deck 미러링";
    const string Disconnected = "도우미와 연결이 끊겼습니다. 도우미가 켜져 있는지 확인하세요";
    // 설정(항상 위, 배율, 창 위치)을 기억하는 곳
    const string RegPath = @"Software\LLMDeck\Mirror";
    // 제목 줄 메뉴(창 왼쪽 위 또는 Alt+Space)에 넣는 항목 번호. 아래 4비트는 Windows 가 쓰므로 16 단위로
    const int MenuTop = 0x10, MenuZoom = 0x20, MenuHome = 0x60;
    const int WM_SYSCOMMAND = 0x112, MF_SEPARATOR = 0x800, MF_CHECKED = 0x8;
    static readonly Color Back = Color.FromArgb(0x0b, 0x0e, 0x11);

    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] static extern IntPtr GetSystemMenu(IntPtr hWnd, bool revert);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool AppendMenu(IntPtr menu, int flags, int id, string text);
    [DllImport("user32.dll")] static extern int CheckMenuItem(IntPtr menu, int id, int flags);

    // 기기가 줄인 화면을 풀 때 쓰는 표: 5비트·6비트 색을 0..255 로 늘린다
    static readonly int[] R5 = new int[32], G6 = new int[64];

    Bitmap screen;
    int w, h;  // 기기 화면 크기 (첫 장이 오기 전에는 0)
    int zoom;
    string state = "기기 화면을 기다리는 중...";  // 화면 위에 띄우는 안내 (null 이면 화면만)
    volatile bool closing;
    // 누르기는 보낸 차례대로 기기에 닿게 한 줄로 보낸다
    readonly BlockingCollection<string[]> outbox = new BlockingCollection<string[]>();
    readonly ContextMenuStrip menu = new ContextMenuStrip();
    readonly ToolStripMenuItem topItem;
    readonly ToolStripMenuItem[] zoomItems = new ToolStripMenuItem[3];
    IntPtr sysMenu;
    bool pressing;
    int lastX, lastY, lastMove, lastSwipe, wheel;

    [STAThread]
    static void Main()
    {
        bool fresh;
        using (Mutex one = new Mutex(true, "LLMDeckMirror", out fresh))
        {
            if (!fresh)
            {
                // 이미 떠 있으면 그 창을 앞으로 가져오고 끝낸다
                ShowOther();
                return;
            }
            // Windows 배율(125%, 150%)로 키우지 않고 기기 1픽셀 = 화면 1픽셀로 그린다. 작으면 2배·3배로
            try { SetProcessDPIAware(); } catch (EntryPointNotFoundException) { }
            ServicePointManager.Expect100Continue = false;  // 보내기 전에 서버 대답을 기다리지 않는다
            ServicePointManager.DefaultConnectionLimit = 8;  // 화면 기다리기와 누르기가 서로 막지 않게
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MirrorForm());
        }
    }

    static void ShowOther()
    {
        Process me = Process.GetCurrentProcess();
        foreach (Process p in Process.GetProcessesByName(me.ProcessName))
        {
            if (p.Id == me.Id || p.MainWindowHandle == IntPtr.Zero) continue;
            if (IsIconic(p.MainWindowHandle)) ShowWindow(p.MainWindowHandle, 9);  // 9 = 최소화 풀기
            SetForegroundWindow(p.MainWindowHandle);
        }
    }

    static MirrorForm()
    {
        for (int i = 0; i < 32; i++) R5[i] = Round(i * 255 / 31.0);
        for (int i = 0; i < 64; i++) G6[i] = Round(i * 255 / 63.0);
    }

    MirrorForm()
    {
        Text = Title;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        AutoScaleMode = AutoScaleMode.None;
        BackColor = Back;
        Font = new Font("Malgun Gothic", 9f);
        Cursor = Cursors.Hand;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        DoubleBuffered = true;
        lastMove = lastSwipe = Environment.TickCount - 1000;

        zoom = Math.Max(1, Math.Min(3, ReadSetting("Zoom", 1)));
        TopMost = ReadSetting("TopMost", 1) != 0;  // 처음에는 항상 위
        int x = ReadSetting("X", int.MinValue), y = ReadSetting("Y", int.MinValue);
        StartPosition = FormStartPosition.CenterScreen;
        foreach (Screen s in Screen.AllScreens)
        {
            // 지난번 자리가 지금 있는 모니터 안이면 거기에 띄운다
            if (x != int.MinValue && s.WorkingArea.Contains(x + 20, y + 20))
            {
                StartPosition = FormStartPosition.Manual;
                Location = new Point(x, y);
            }
        }
        Fit();

        // 오른쪽 클릭 메뉴
        topItem = new ToolStripMenuItem("항상 위", null, (s, e) => SetTopMost(!TopMost));
        menu.Items.Add(topItem);
        menu.Items.Add(new ToolStripSeparator());
        for (int i = 0; i < 3; i++)
        {
            int z = i + 1;
            zoomItems[i] = new ToolStripMenuItem(z + "배", null, (s, e) => SetZoom(z));
            menu.Items.Add(zoomItems[i]);
        }
        menu.Items.Add(new ToolStripSeparator());
        ToolStripMenuItem home = new ToolStripMenuItem("홈", null, (s, e) => Post("home", "{}"));
        home.ShortcutKeyDisplayString = "Esc";
        menu.Items.Add(home);
        menu.Items.Add(new ToolStripMenuItem("닫기", null, (s, e) => Close()));
        ContextMenuStrip = menu;
        Checks();
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        // 제목 줄 메뉴에도 같은 항목을 둔다 (창이 작아 메뉴 막대 대신)
        sysMenu = GetSystemMenu(Handle, false);
        AppendMenu(sysMenu, MF_SEPARATOR, 0, null);
        AppendMenu(sysMenu, 0, MenuTop, "항상 위");
        for (int z = 1; z <= 3; z++) AppendMenu(sysMenu, 0, MenuZoom + (z - 1) * 0x10, z + "배");
        AppendMenu(sysMenu, 0, MenuHome, "홈\tEsc");
        Checks();
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        Thread poll = new Thread(Poll);
        poll.IsBackground = true;
        poll.Start();
        Thread send = new Thread(Send);
        send.IsBackground = true;
        send.Start();
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        closing = true;
        outbox.CompleteAdding();
        if (WindowState == FormWindowState.Normal)
        {
            Save("X", Left);
            Save("Y", Top);
        }
        base.OnFormClosing(e);
    }

    protected override void WndProc(ref Message m)
    {
        if (m.Msg == WM_SYSCOMMAND)
        {
            int id = (int)((long)m.WParam & 0xFFF0);
            if (id == MenuTop) { SetTopMost(!TopMost); return; }
            if (id >= MenuZoom && id < MenuZoom + 3 * 0x10) { SetZoom((id - MenuZoom) / 0x10 + 1); return; }
            if (id == MenuHome) { Post("home", "{}"); return; }
        }
        base.WndProc(ref m);
    }

    void SetTopMost(bool on)
    {
        TopMost = on;
        Save("TopMost", on ? 1 : 0);
        Checks();
    }

    void SetZoom(int z)
    {
        zoom = z;
        Save("Zoom", z);
        Fit();
        Checks();
        Invalidate();
    }

    // 메뉴의 체크 표시를 지금 설정에 맞춘다
    void Checks()
    {
        topItem.Checked = TopMost;
        for (int i = 0; i < 3; i++) zoomItems[i].Checked = zoom == i + 1;
        if (sysMenu == IntPtr.Zero) return;
        CheckMenuItem(sysMenu, MenuTop, TopMost ? MF_CHECKED : 0);
        for (int i = 0; i < 3; i++) CheckMenuItem(sysMenu, MenuZoom + i * 0x10, zoom == i + 1 ? MF_CHECKED : 0);
    }

    // 창 안쪽 크기 = 기기 화면 크기 × 배율 (첫 장이 오기 전에는 320x240)
    void Fit()
    {
        ClientSize = new Size((w > 0 ? w : 320) * zoom, (h > 0 ? h : 240) * zoom);
    }

    protected override void OnPaintBackground(PaintEventArgs e)
    {
        // 깜빡이지 않게 OnPaint 에서 한 번에 칠한다
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics;
        g.Clear(Back);
        if (screen != null)
        {
            // 확대해도 흐려지지 않게 가장 가까운 점으로 키운다
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            g.PixelOffsetMode = PixelOffsetMode.Half;
            g.DrawImage(screen, new Rectangle(0, 0, w * zoom, h * zoom), 0, 0, w, h, GraphicsUnit.Pixel);
        }
        if (state != null)
        {
            if (screen != null)
            {
                using (SolidBrush dim = new SolidBrush(Color.FromArgb(200, Back))) g.FillRectangle(dim, ClientRectangle);
            }
            Rectangle box = ClientRectangle;
            box.Inflate(-12, -12);
            TextRenderer.DrawText(g, state, Font, box, Color.FromArgb(0xc8, 0xd0, 0xd8),
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.WordBreak);
        }
    }

    // 다른 스레드에서 창을 고칠 때는 창 스레드로 넘긴다
    void Ui(MethodInvoker action)
    {
        try
        {
            if (!closing && IsHandleCreated) BeginInvoke(action);
        }
        catch (InvalidOperationException) { }  // 창을 닫는 중
    }

    void SetState(string text)
    {
        Ui(() =>
        {
            if (state == text) return;
            state = text;
            Invalidate();
        });
    }

    void ShowFrame(int fw, int fh, int[] pixels)
    {
        if (screen == null || fw != w || fh != h)
        {
            // 처음이거나 기기 화면을 돌렸다 (320x240 ↔ 240x320)
            if (screen != null) screen.Dispose();
            screen = new Bitmap(fw, fh, PixelFormat.Format32bppRgb);
            w = fw;
            h = fh;
            Fit();
        }
        BitmapData data = screen.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppRgb);
        try
        {
            for (int y = 0; y < h; y++) Marshal.Copy(pixels, y * w, new IntPtr(data.Scan0.ToInt64() + (long)y * data.Stride), w);
        }
        finally
        {
            screen.UnlockBits(data);
        }
        state = null;
        Invalidate();
    }

    static HttpWebRequest Request(string path)
    {
        HttpWebRequest req = (HttpWebRequest)WebRequest.Create(Base + path);
        req.Proxy = null;  // 회사 프록시 설정이 있어도 이 PC 안으로 바로 간다
        req.KeepAlive = false;
        return req;
    }

    // 새 장이 올 때까지 기다렸다 받기를 되풀이한다 (도우미가 1.5초까지 붙잡고 있다가 204 로 "새 장 없음")
    void Poll()
    {
        int seq = 0;
        while (!closing)
        {
            try
            {
                HttpWebRequest req = Request("frame?after=" + seq);
                req.Timeout = 15000;
                req.ReadWriteTimeout = 15000;
                using (HttpWebResponse res = (HttpWebResponse)req.GetResponse())
                {
                    if (res.StatusCode != HttpStatusCode.OK) continue;
                    int next = int.Parse(res.Headers["X-Seq"]);
                    int fw = int.Parse(res.Headers["X-Width"]), fh = int.Parse(res.Headers["X-Height"]);
                    byte[] body = ReadAll(res.GetResponseStream());
                    if (fw <= 0 || fh <= 0 || fw * fh > 4096 * 4096) throw new InvalidDataException();
                    int[] pixels = Unpack(body, fw * fh);
                    seq = next;
                    Ui(() => ShowFrame(fw, fh, pixels));
                }
            }
            catch (WebException e)
            {
                HttpWebResponse res = e.Response as HttpWebResponse;
                if (res != null && res.StatusCode == HttpStatusCode.ServiceUnavailable)
                {
                    // 기기가 USB·Wi-Fi 로 연결되어 있지 않다. 도우미가 보낸 안내를 그대로 보여 준다 (도우미가 1초 쉬고 대답한다)
                    using (StreamReader reader = new StreamReader(res.GetResponseStream(), Encoding.UTF8)) SetState(reader.ReadToEnd());
                    res.Close();
                }
                else
                {
                    if (res != null) res.Close();
                    SetState(Disconnected);
                    Thread.Sleep(2000);
                }
            }
            catch (Exception)
            {
                // 받은 장이 이상하거나 그 밖의 문제. 브라우저 페이지처럼 잠시 쉬고 다시 받는다
                SetState(Disconnected);
                Thread.Sleep(2000);
            }
        }
    }

    static byte[] ReadAll(Stream stream)
    {
        using (MemoryStream all = new MemoryStream())
        {
            stream.CopyTo(all);
            return all.ToArray();
        }
    }

    // 기기가 줄인 화면을 푼다 (펌웨어 mirror.cpp 의 pack): 머리 h < 128 이면 h+1 점 그대로, 아니면 한 점을 h-126 번.
    // 한 점은 2바이트 작은 끝 RGB565
    static int[] Unpack(byte[] src, int n)
    {
        int[] d = new int[n];
        int o = 0, p = 0;
        while (o < src.Length && p < n)
        {
            int head = src[o++];
            if (head < 128)
            {
                for (int i = 0; i <= head && p < n && o + 1 < src.Length; i++, o += 2) d[p++] = Color565(src[o] | (src[o + 1] << 8));
            }
            else
            {
                if (o + 1 >= src.Length) break;
                int c = Color565(src[o] | (src[o + 1] << 8));
                o += 2;
                for (int i = 0; i < head - 126 && p < n; i++) d[p++] = c;
            }
        }
        return d;
    }

    static int Color565(int v)
    {
        return unchecked((int)0xff000000) | (R5[v >> 11] << 16) | (G6[(v >> 5) & 63] << 8) | R5[v & 31];
    }

    // 누르기를 한 줄로 보낸다. 도우미가 꺼져 있으면 버린다 (화면에 안내가 뜬다)
    void Send()
    {
        foreach (string[] job in outbox.GetConsumingEnumerable())
        {
            try
            {
                HttpWebRequest req = Request(job[0]);
                req.Method = "POST";
                req.ContentType = "application/json";  // 도우미가 이 형식만 받는다
                req.Timeout = 5000;
                byte[] body = Encoding.UTF8.GetBytes(job[1]);
                req.ContentLength = body.Length;
                using (Stream s = req.GetRequestStream()) s.Write(body, 0, body.Length);
                req.GetResponse().Close();
            }
            catch (WebException) { }
            catch (IOException) { }
        }
    }

    void Post(string path, string json)
    {
        if (closing) return;
        try { outbox.Add(new string[] { path, json }); }
        catch (InvalidOperationException) { }  // 창을 닫는 중
    }

    static string Json(int x, int y, bool down)
    {
        return "{\"x\":" + x + ",\"y\":" + y + ",\"down\":" + (down ? "true" : "false") + "}";
    }

    static int Round(double v)
    {
        return (int)Math.Floor(v + 0.5);  // 자바스크립트 Math.round 와 같게 (.5 는 올림)
    }

    // 창 위치를 기기 픽셀로 바꿔 보낸다. 창 밖으로 끌면 가장자리로 자른다
    void Touch(int cx, int cy, bool down)
    {
        if (w == 0) return;
        lastX = cx;
        lastY = cy;
        int x = Math.Max(0, Math.Min(w - 1, (int)Math.Floor(cx / (double)zoom)));
        int y = Math.Max(0, Math.Min(h - 1, (int)Math.Floor(cy / (double)zoom)));
        Post("touch", Json(x, y, down));
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (e.Button != MouseButtons.Left) return;  // 오른쪽은 메뉴
        pressing = true;
        Touch(e.X, e.Y, true);
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (!pressing || Environment.TickCount - lastMove < 30) return;
        lastMove = Environment.TickCount;
        Touch(e.X, e.Y, true);
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        if (e.Button != MouseButtons.Left || !pressing) return;
        pressing = false;
        Touch(e.X, e.Y, false);
    }

    protected override void OnMouseCaptureChanged(EventArgs e)
    {
        base.OnMouseCaptureChanged(e);
        // 누른 채로 다른 창으로 넘어가면 그 자리에서 뗀다
        if (!pressing) return;
        pressing = false;
        Touch(lastX, lastY, false);
    }

    // 밀기: 화면 가운데를 지나 한쪽에서 다른 쪽으로 끄는 점들 (기기가 10ms 마다 하나씩 쓴다)
    void Swipe(int dx, int dy)
    {
        if (w == 0) return;
        double x0 = w / 2.0 - dx * w * 0.35, y0 = h / 2.0 - dy * h * 0.3;
        StringBuilder points = new StringBuilder("{\"points\":[");
        int x = 0, y = 0;
        for (int i = 0; i <= 8; i++)
        {
            x = Round(x0 + dx * w * 0.7 * i / 8);
            y = Round(y0 + dy * h * 0.6 * i / 8);
            points.Append(Json(x, y, true)).Append(',');
        }
        points.Append(Json(x, y, false)).Append("]}");
        Post("touch", points.ToString());
    }

    protected override void OnMouseWheel(MouseEventArgs e)
    {
        base.OnMouseWheel(e);
        wheel -= e.Delta;  // 브라우저처럼 아래로 굴리면 +
        int now = Environment.TickCount;
        if (Math.Abs(wheel) < 40 || now - lastSwipe < 350) return;
        Swipe(0, wheel > 0 ? -1 : 1);  // 아래로 굴리면 다음 쪽 (손가락을 위로 미는 것)
        wheel = 0;
        lastSwipe = now;
    }

    // 방향키로 밀기, Esc 로 홈. 키를 누르고 있어도 한 번만 민다
    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        bool repeat = ((long)msg.LParam & 0x40000000) != 0;
        switch (keyData)
        {
            case Keys.Down: if (!repeat) Swipe(0, -1); return true;
            case Keys.Up: if (!repeat) Swipe(0, 1); return true;
            case Keys.Right: if (!repeat) Swipe(-1, 0); return true;
            case Keys.Left: if (!repeat) Swipe(1, 0); return true;
            case Keys.Escape: Post("home", "{}"); return true;
        }
        return base.ProcessCmdKey(ref msg, keyData);
    }

    static int ReadSetting(string name, int fallback)
    {
        try
        {
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(RegPath))
            {
                if (key != null)
                {
                    object v = key.GetValue(name);
                    if (v is int) return (int)v;
                }
            }
        }
        catch (Exception) { }
        return fallback;
    }

    static void Save(string name, int value)
    {
        try
        {
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(RegPath)) key.SetValue(name, value, RegistryValueKind.DWord);
        }
        catch (Exception) { }
    }
}
