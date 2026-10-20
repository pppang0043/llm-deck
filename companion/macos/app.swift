// LLM Deck.app 의 실행 파일. install.sh 가 swiftc 로 컴파일한다.
// 블루투스·마이크 권한은 "앱" 단위로만 받을 수 있어서, 파이썬 도우미(main.py)를 이 앱의 자식 프로세스로 띄워 둔다.
// 메뉴 막대 아이콘에서 미러링 창을 연다. 앱이 이미 떠 있을 때 한 번 더 열어도 (Finder, Spotlight, `uv run main.py --mirror`) 연다.
// 미러링 창은 도우미가 띄운 http://127.0.0.1:4013/ (mirror.py) 에서 화면을 받아 오고 누르기를 보낸다. 서버 쪽 규칙은 mirror.py 의 PAGE 와 같다.
// 창이 열려 있는 동안(화면을 받아 가는 동안)만 기기가 화면을 보낸다.
import AppKit

let BASE = URL(string: "http://127.0.0.1:4013/")!
let LOG = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".llm-deck/companion.log")
let OFFLINE = "도우미와 연결이 끊겼습니다. 도우미가 켜져 있는지 확인하세요"

// 기기가 줄인 화면을 푼다 (펌웨어 mirror.cpp 의 pack): 머리 h < 128 이면 h+1 점 그대로, 아니면 한 점을 h-126 번
let R: [UInt32] = (0..<32).map { UInt32((Double($0) * 255 / 31).rounded()) }
let G: [UInt32] = (0..<64).map { UInt32((Double($0) * 255 / 63).rounded()) }

func unpack(_ src: Data, _ w: Int, _ h: Int) -> CGImage? {
  let n = w * h
  var px = [UInt32](repeating: 0xff00_0000, count: n)
  src.withUnsafeBytes { (s: UnsafeRawBufferPointer) in
    func color(_ o: Int) -> UInt32 {
      let v = Int(s[o]) | (o + 1 < s.count ? Int(s[o + 1]) << 8 : 0)
      return 0xff00_0000 | R[v >> 11] << 16 | G[(v >> 5) & 63] << 8 | R[v & 31]
    }
    var o = 0, p = 0
    while o < s.count && p < n {
      let head = Int(s[o])
      o += 1
      if head < 128 {
        var i = 0
        while i <= head && p < n && o < s.count {
          px[p] = color(o)
          p += 1; i += 1; o += 2
        }
      } else {
        guard o < s.count else { break }
        let c = color(o)
        o += 2
        for _ in 0..<min(head - 126, n - p) {
          px[p] = c
          p += 1
        }
      }
    }
  }
  // 0xAARRGGBB 를 작은 끝으로 둔 32비트 그림
  guard let data = CGDataProvider(data: Data(bytes: px, count: n * 4) as CFData) else { return nil }
  return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                 space: CGColorSpaceCreateDeviceRGB(),
                 bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue),
                 provider: data, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
}

// 기기 화면을 그리고 마우스·키보드를 받는 칸. 크기는 늘 (기기 폭 × 배율, 기기 높이 × 배율)
@MainActor
final class ScreenView: NSView {
  weak var app: Mirror?
  let label = NSTextField(wrappingLabelWithString: "기기 화면을 기다리는 중...")
  var image: CGImage? { didSet { needsDisplay = true } }
  var down = false
  var lastMove = 0.0
  var lastSwipe = 0.0
  var wheel = 0.0

  override init(frame: NSRect) {
    super.init(frame: frame)
    wantsLayer = true
    layerContentsRedrawPolicy = .onSetNeedsDisplay
    label.alignment = .center
    label.textColor = NSColor(red: 0.54, green: 0.58, blue: 0.62, alpha: 1)
    label.font = .systemFont(ofSize: 13)
    label.drawsBackground = true
    label.backgroundColor = NSColor(white: 0, alpha: 0.6)
    addSubview(label)
  }

  required init?(coder: NSCoder) { fatalError() }

  override var isFlipped: Bool { true }
  override var wantsUpdateLayer: Bool { true }

  override func updateLayer() {
    guard let layer else { return }
    layer.backgroundColor = NSColor(red: 0.04, green: 0.05, blue: 0.07, alpha: 1).cgColor
    layer.contentsGravity = .resize
    layer.magnificationFilter = .nearest  // 키워도 점이 번지지 않게
    layer.contents = image
  }
  override var acceptsFirstResponder: Bool { true }
  override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

  // 안내 글은 칸 가운데에 (빈 글이면 숨긴다)
  func say(_ text: String?) {
    label.isHidden = text == nil
    label.stringValue = text ?? ""
    needsLayout = true
  }

  override func layout() {
    super.layout()
    let width = max(bounds.width - 16, 40)
    label.preferredMaxLayoutWidth = width
    let size = label.sizeThatFits(NSSize(width: width, height: .greatestFiniteMagnitude))
    label.frame = NSRect(x: (bounds.width - size.width) / 2, y: (bounds.height - size.height) / 2, width: size.width, height: size.height)
  }

  // 창의 점 → 기기 픽셀 (화면 밖은 가장자리로)
  func point(_ e: NSEvent) -> (Int, Int) {
    guard let app else { return (0, 0) }
    let p = convert(e.locationInWindow, from: nil)
    let x = Int((p.x * CGFloat(app.w) / bounds.width).rounded(.down))
    let y = Int((p.y * CGFloat(app.h) / bounds.height).rounded(.down))
    return (min(max(x, 0), app.w - 1), min(max(y, 0), app.h - 1))
  }

  func send(_ e: NSEvent, _ isDown: Bool) {
    guard let app, app.got else { return }
    let (x, y) = point(e)
    app.post("touch", ["x": x, "y": y, "down": isDown])
  }

  override func mouseDown(with e: NSEvent) {
    // 컨트롤 + 클릭은 오른쪽 메뉴
    if e.modifierFlags.contains(.control), let menu = menu(for: e) {
      NSMenu.popUpContextMenu(menu, with: e, for: self)
      return
    }
    down = true
    send(e, true)
  }

  override func mouseDragged(with e: NSEvent) {
    if !down || e.timestamp - lastMove < 0.03 { return }
    lastMove = e.timestamp
    send(e, true)
  }

  override func mouseUp(with e: NSEvent) {
    if down {
      down = false
      send(e, false)
    }
  }

  override func scrollWheel(with e: NSEvent) {
    // 브라우저의 wheel 처럼 아래로 굴리면 +. 마우스 휠은 한 칸이 1 이라 40 을 곱한다
    wheel -= e.hasPreciseScrollingDeltas ? e.scrollingDeltaY : e.scrollingDeltaY * 40
    if abs(wheel) < 40 || e.timestamp - lastSwipe < 0.35 { return }
    app?.swipe(0, wheel > 0 ? -1 : 1)  // 아래로 굴리면 다음 쪽 (손가락을 위로 미는 것)
    wheel = 0
    lastSwipe = e.timestamp
  }

  override func keyDown(with e: NSEvent) {
    let moves: [UInt16: (Double, Double)] = [125: (0, -1), 126: (0, 1), 124: (-1, 0), 123: (1, 0)]  // ↓ ↑ → ←
    if let m = moves[e.keyCode] {
      if !e.isARepeat { app?.swipe(m.0, m.1) }
    } else if e.keyCode == 53 {
      app?.home()
    } else {
      super.keyDown(with: e)
    }
  }

  override func menu(for event: NSEvent) -> NSMenu? { app?.makeMenu() }
}

@MainActor
final class Mirror: NSObject, NSWindowDelegate, NSMenuItemValidation {
  let defaults = UserDefaults.standard
  let session: URLSession
  var window: NSWindow?
  let view = ScreenView(frame: NSRect(x: 0, y: 0, width: 320, height: 240))
  // 첫 장이 오기 전에는 320x240 으로 둔다
  var w = 320, h = 240, seq = 0
  var got = false
  var sending: Task<Void, Never>?
  var polling: Task<Void, Never>?
  // 창이 열려 있는 동안은 다른 앱을 쓰고 있어도(항상 위) macOS 가 이 앱을 쉬게 하거나(App Nap) 화면 받기를 늦추지 않게 한다
  var awake: NSObjectProtocol?

  override init() {
    let config = URLSessionConfiguration.ephemeral
    config.requestCachePolicy = .reloadIgnoringLocalCacheData
    config.timeoutIntervalForRequest = 10  // 서버는 1.5초 안에 답한다
    session = URLSession(configuration: config)
    super.init()
    // 예전 미러링 앱(LLM Deck Mirror.app)에 저장해 둔 배율·항상 위·창 위치를 한 번 옮겨 온다
    if defaults.object(forKey: "zoom") == nil, let old = UserDefaults(suiteName: "com.llmdeck.mirror") {
      for key in ["zoom", "onTop", "NSWindow Frame mirror"] {
        if let v = old.object(forKey: key) { defaults.set(v, forKey: key) }
      }
    }
    defaults.register(defaults: ["onTop": true, "zoom": 1])
  }

  var zoom: Int { min(max(defaults.integer(forKey: "zoom"), 1), 3) }
  var onTop: Bool { defaults.bool(forKey: "onTop") }

  // 창을 연다 (열려 있으면 앞으로). 창이 떠 있는 동안만 Dock 아이콘과 메뉴 막대 메뉴를 보인다
  @objc func open() {
    let window = self.window ?? makeWindow()
    NSApp.setActivationPolicy(.regular)
    NSApp.mainMenu = makeMainMenu()
    fit()
    applyOnTop()
    window.makeKeyAndOrderFront(nil)
    window.makeFirstResponder(view)
    if #available(macOS 14, *) { NSApp.activate() } else { NSApp.activate(ignoringOtherApps: true) }
    if polling == nil {
      awake = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .latencyCritical], reason: "기기 화면 미러링")
      polling = Task { await poll() }
    }
  }

  func makeWindow() -> NSWindow {
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 320, height: 240), styleMask: [.titled, .closable, .miniaturizable],
                          backing: .buffered, defer: false)
    window.title = "LLM Deck 미러링"
    window.isReleasedWhenClosed = false
    window.contentView = view
    window.delegate = self
    view.app = self
    window.center()
    window.setFrameAutosaveName("mirror")  // 지난번 창 위치를 기억한다
    self.window = window
    return window
  }

  // 창을 닫으면 화면 받기를 멈춘다 (도우미가 기기에 미러링을 그만 보내게). 도우미는 계속 돈다
  func windowWillClose(_ note: Notification) {
    polling?.cancel()
    polling = nil
    if let awake { ProcessInfo.processInfo.endActivity(awake) }
    awake = nil
    NSApp.setActivationPolicy(.accessory)
  }

  // 창 내용 크기를 기기 화면 × 배율로 맞춘다 (창 왼쪽 위 모서리는 그대로)
  func fit() {
    guard let window else { return }
    let size = NSSize(width: w * zoom, height: h * zoom)
    let now = window.frame
    if window.contentRect(forFrameRect: now).size == size { return }
    var frame = window.frameRect(forContentRect: NSRect(origin: .zero, size: size))
    frame.origin = NSPoint(x: now.minX, y: now.maxY - frame.height)
    window.setFrame(frame, display: true)
  }

  func applyOnTop() {
    window?.level = onTop ? .floating : .normal
  }

  // 새 장을 계속 받아 간다. 이 요청이 이어지는 동안만 도우미가 기기에 화면을 보내 달라고 한다
  func poll() async {
    while !Task.isCancelled {
      var req = URLRequest(url: URL(string: "frame?after=\(seq)", relativeTo: BASE)!)
      req.cachePolicy = .reloadIgnoringLocalCacheData
      do {
        let (data, res) = try await session.data(for: req)
        let r = res as! HTTPURLResponse
        if r.statusCode == 200 {
          let fw = Int(r.value(forHTTPHeaderField: "X-Width") ?? "") ?? 0
          let fh = Int(r.value(forHTTPHeaderField: "X-Height") ?? "") ?? 0
          seq = Int(r.value(forHTTPHeaderField: "X-Seq") ?? "") ?? 0
          if fw > 0 && fh > 0, let image = unpack(data, fw, fh) {
            (w, h, got) = (fw, fh, true)
            view.image = image
            fit()
            view.say(nil)
          }
        } else if r.statusCode == 503 {
          view.say(String(decoding: data, as: UTF8.self))
        } else if r.statusCode != 204 {
          try? await Task.sleep(for: .seconds(1))  // 모르는 답이면 너무 자주 묻지 않게
        }
      } catch {
        if Task.isCancelled { break }
        view.say(OFFLINE)
        try? await Task.sleep(for: .seconds(2))
      }
    }
  }

  // 보낸 차례대로 도우미에 닿게 하나씩 보낸다
  func post(_ path: String, _ body: [String: Any]) {
    var req = URLRequest(url: URL(string: path, relativeTo: BASE)!)
    req.httpMethod = "POST"
    req.setValue("application/json", forHTTPHeaderField: "Content-Type")
    req.httpBody = try? JSONSerialization.data(withJSONObject: body)
    let before = sending, session = session
    sending = Task {
      await before?.value
      _ = try? await session.data(for: req)
    }
  }

  // 밀기: 화면 가운데를 지나 한쪽에서 다른 쪽으로 끄는 점들 (기기가 10ms 마다 하나씩 쓴다)
  func swipe(_ dx: Double, _ dy: Double) {
    if !got { return }
    let fw = Double(w), fh = Double(h)
    let x0 = fw / 2 - dx * fw * 0.35, y0 = fh / 2 - dy * fh * 0.3
    var points: [[String: Any]] = (0...8).map { i in
      ["x": Int((x0 + dx * fw * 0.7 * Double(i) / 8).rounded()), "y": Int((y0 + dy * fh * 0.6 * Double(i) / 8).rounded()), "down": true]
    }
    var last = points[8]
    last["down"] = false
    points.append(last)
    post("touch", ["points": points])
  }

  @objc func home() { post("home", [:]) }

  @objc func toggleOnTop() {
    defaults.set(!onTop, forKey: "onTop")
    applyOnTop()
  }

  @objc func setZoom(_ item: NSMenuItem) {
    defaults.set(item.tag, forKey: "zoom")
    fit()
  }

  // 메뉴 막대와 오른쪽 메뉴에 같이 넣는 항목들
  func items() -> [NSMenuItem] {
    func item(_ title: String, _ action: Selector, _ key: String, tag: Int = 0) -> NSMenuItem {
      let it = NSMenuItem(title: title, action: action, keyEquivalent: key)
      it.target = self
      it.tag = tag
      return it
    }
    let esc = item("홈 (Esc)", #selector(home), "\u{1b}")
    esc.keyEquivalentModifierMask = []
    return [
      item("항상 위", #selector(toggleOnTop), "t"),
      .separator(),
      item("1배", #selector(setZoom(_:)), "1", tag: 1),
      item("2배", #selector(setZoom(_:)), "2", tag: 2),
      item("3배", #selector(setZoom(_:)), "3", tag: 3),
      .separator(),
      esc,
    ]
  }

  func makeMenu() -> NSMenu {
    let menu = NSMenu()
    for it in items() { menu.addItem(it) }
    menu.addItem(.separator())
    // 종료는 두지 않는다 (도우미까지 꺼진다). 창을 닫아도 도우미는 메뉴 막대에 남는다
    menu.addItem(withTitle: "미러링 닫기", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
    return menu
  }

  func makeMainMenu() -> NSMenu {
    let bar = NSMenu()
    let top = NSMenuItem()
    top.submenu = makeMenu()
    bar.addItem(top)
    return bar
  }

  // 메뉴를 열 때마다 체크 표시를 지금 설정에 맞춘다
  func validateMenuItem(_ item: NSMenuItem) -> Bool {
    if item.action == #selector(toggleOnTop) {
      item.state = onTop ? .on : .off
    } else if item.action == #selector(setZoom(_:)) {
      item.state = item.tag == zoom ? .on : .off
    }
    return true
  }
}

// ---- 도우미 (파이썬) ----

// 기록 파일이 이보다 커지면 도우미를 띄울 때 companion.log.1 로 넘기고 새로 쓴다 (하나만 남긴다)
let LOG_LIMIT = 2_000_000

// 앱의 출력(launchd 가 StandardOutPath 로 기록 파일에 이어 둔다)을 새 파일로 바꾼다. 도우미는 이것을 물려받는다.
// 터미널에서 띄웠을 때처럼 출력이 기록 파일이 아니면 건드리지 않는다
func rotateLog() {
  var out = stat(), file = stat()
  guard fstat(1, &out) == 0, stat(LOG.path, &file) == 0, out.st_ino == file.st_ino, out.st_dev == file.st_dev,
        file.st_size > LOG_LIMIT else { return }
  let old = LOG.path + ".1"
  guard rename(LOG.path, old) == 0 else { return }
  let fd = open(LOG.path, O_WRONLY | O_CREAT | O_APPEND, 0o644)
  guard fd >= 0 else { return }
  fflush(stdout)
  dup2(fd, 1)
  dup2(fd, 2)
  close(fd)
  log("[app] 기록이 커서 지난 기록은 \(old) 로 옮겼습니다")
}

// 출력이 파일이면 print 가 모아 두었다가 쓰므로 바로 내보낸다
func log(_ text: String) {
  print(text)
  fflush(stdout)
}

// 앱이 받은 인자는 도우미에게 그대로 넘긴다 (예: --link ble). 도우미가 제대로 끝나면 앱도 끝난다 (그러면 launchd 가 다시 띄운다).
// 오류로 끝나면(설정 파일이 틀림, .venv 없음 등) 앱은 남아서 메뉴 막대 아이콘으로 알리고, 30초마다 다시 띄워 본다
@MainActor
final class Helper {
  var process: Process?
  var restarting = false
  var stopping = false  // 종료 신호를 넘겼다 (끝나면 앱도 끝낸다)
  var failed = false
  // 오류 상태가 바뀔 때 (메뉴 막대 아이콘을 바꾼다)
  var onFailed: (Bool) -> Void = { _ in }

  func start() {
    rotateLog()
    let dir = Bundle.main.object(forInfoDictionaryKey: "LLMDeckCompanionDir") as? String ?? ""  // install.sh 가 넣어 준다
    let p = Process()
    p.currentDirectoryURL = URL(fileURLWithPath: dir)
    p.executableURL = URL(fileURLWithPath: dir + "/.venv/bin/python")
    p.arguments = ["main.py"] + CommandLine.arguments.dropFirst().filter { !$0.hasPrefix("-psn_") }
    var env = ProcessInfo.processInfo.environment
    env["PYTHONUNBUFFERED"] = "1"
    env["LLM_DECK_APP_PID"] = String(getpid())  // 이 앱이 SIGUSR1 을 받으면 미러링 창을 연다
    env["PATH"] = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin"
    p.environment = env
    p.terminationHandler = { p in
      let code = p.terminationReason == .exit ? p.terminationStatus : 1
      Task { @MainActor in self.exited(code) }
    }
    do {
      try p.run()
      process = p
    } catch {
      log("[app] 도우미를 띄우지 못했습니다 (\(dir)): \(error)")
      fail()
      return
    }
    // 한동안 돌면 오류 표시를 거둔다 (설정 파일 오류 같은 것은 띄우자마자 끝난다)
    Task { @MainActor in
      try? await Task.sleep(for: .seconds(15))
      if self.failed, self.process === p, p.isRunning { self.setFailed(false) }
    }
  }

  func exited(_ code: Int32) {
    if restarting {
      restarting = false
      start()
    } else if code == 0 || stopping {
      exit(code)
    } else {
      log("[app] 도우미가 오류로 끝났습니다 (코드 \(code)). 30초 뒤 다시 띄웁니다")
      fail()
    }
  }

  // 메뉴 막대에 오류를 보이고 30초 뒤 다시 띄운다 (예전에는 앱이 끝나 launchd 가 30초마다 다시 띄웠다)
  func fail() {
    process = nil
    setFailed(true)
    Task { @MainActor in
      try? await Task.sleep(for: .seconds(30))
      if self.process == nil && !self.stopping { self.start() }
    }
  }

  func setFailed(_ value: Bool) {
    failed = value
    onFailed(value)
  }

  @objc func restart() {
    guard let process, process.isRunning else {
      if self.process == nil { start() }  // 오류로 끝나 있으면 바로 다시 띄운다
      return
    }
    log("[app] 도우미를 다시 시작합니다")
    restarting = true
    process.terminate()
  }

  // 앱이 꺼질 때 도우미도 같이 끝낸다 (USB 포트를 붙잡은 채 남지 않도록)
  func stop() {
    guard let process, process.isRunning else { return }
    process.terminate()
    let until = Date().addingTimeInterval(3)
    while process.isRunning && Date() < until { usleep(50_000) }
  }
}

@MainActor
final class App: NSObject, NSApplicationDelegate {
  let helper = Helper()
  let mirror = Mirror()
  var status: NSStatusItem?
  var errorItem: NSMenuItem?
  var signals: [DispatchSourceSignal] = []

  func applicationDidFinishLaunching(_ note: Notification) {
    // 종료 신호(launchctl bootout, Ctrl+C)는 도우미에게 넘긴다. 도우미가 끝나면 앱도 끝난다
    for sig in [SIGTERM, SIGINT, SIGHUP] {
      signal(sig, SIG_IGN)
      let source = DispatchSource.makeSignalSource(signal: sig, queue: .main)
      source.setEventHandler {
        MainActor.assumeIsolated {
          self.helper.stopping = true
          if let p = self.helper.process, p.isRunning { kill(p.processIdentifier, sig) } else { exit(0) }
        }
      }
      source.resume()
      signals.append(source)
    }
    // 도우미가 기기와 새로 연결되면 SIGUSR1 로 미러링 창을 열게 한다 (mirror.py 의 open_window)
    signal(SIGUSR1, SIG_IGN)
    let open = DispatchSource.makeSignalSource(signal: SIGUSR1, queue: .main)
    open.setEventHandler { MainActor.assumeIsolated { self.mirror.open() } }
    open.resume()
    signals.append(open)
    let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    item.button?.image = NSImage(systemSymbolName: "rectangle.on.rectangle", accessibilityDescription: "LLM Deck")
    item.button?.toolTip = "LLM Deck"
    let menu = NSMenu()
    func add(_ title: String, _ target: AnyObject, _ action: Selector) {
      let it = menu.addItem(withTitle: title, action: action, keyEquivalent: "")
      it.target = target
    }
    add("미러링 열기", mirror, #selector(Mirror.open))
    add("항상 위", mirror, #selector(Mirror.toggleOnTop))
    menu.addItem(.separator())
    add("도우미 다시 시작", helper, #selector(Helper.restart))
    add("기록 보기", self, #selector(showLog))
    item.menu = menu
    status = item
    helper.onFailed = { [weak self] failed in self?.showFailed(failed) }
    helper.start()
  }

  // 도우미가 오류로 끝나 있으면 아이콘을 경고 표시로 바꾸고 메뉴 맨 위에 알린다
  func showFailed(_ failed: Bool) {
    guard let status, let menu = status.menu else { return }
    let symbol = failed ? "exclamationmark.triangle" : "rectangle.on.rectangle"
    status.button?.image = NSImage(systemSymbolName: symbol, accessibilityDescription: "LLM Deck")
    status.button?.toolTip = failed ? "LLM Deck: 도우미가 오류로 멈췄습니다 (30초마다 다시 띄웁니다)" : "LLM Deck"
    if failed && errorItem == nil {
      let it = NSMenuItem(title: "도우미 오류 — 기록 보기", action: #selector(showLog), keyEquivalent: "")
      it.target = self
      menu.insertItem(it, at: 0)
      menu.insertItem(.separator(), at: 1)
      errorItem = it
    } else if !failed, let it = errorItem {
      let index = menu.index(of: it)
      menu.removeItem(at: index + 1)  // 아래 구분선
      menu.removeItem(it)
      errorItem = nil
    }
  }

  // 이미 떠 있는 앱을 한 번 더 열면 미러링 창을 연다
  func applicationShouldHandleReopen(_ app: NSApplication, hasVisibleWindows: Bool) -> Bool {
    mirror.open()
    return false
  }

  func applicationWillTerminate(_ note: Notification) { helper.stop() }

  @objc func showLog() { NSWorkspace.shared.open(LOG) }
}

MainActor.assumeIsolated {
  let app = NSApplication.shared
  let delegate = App()
  app.delegate = delegate
  app.setActivationPolicy(.accessory)
  app.run()
}
