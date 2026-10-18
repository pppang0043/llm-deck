// LLM Deck Mirror.app 의 실행 파일. 브라우저 대신 작은 창에 기기 화면을 보여 주고, 눌러서 기기를 조작한다.
// 도우미가 띄운 http://127.0.0.1:4013/ (mirror.py) 에서 화면을 받아 오고 누르기를 보낸다. 서버 쪽 규칙은 mirror.py 의 PAGE 와 같다.
// 창이 열려 있는 동안(화면을 받아 가는 동안)만 기기가 화면을 보낸다. install.sh 가 swiftc 로 컴파일한다.
import AppKit

let BASE = URL(string: "http://127.0.0.1:4013/")!
let BUNDLE_ID = "com.llmdeck.mirror"
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
final class Mirror: NSObject, NSApplicationDelegate, NSMenuItemValidation {
  let defaults = UserDefaults.standard
  let session: URLSession
  var window: NSWindow!
  let view = ScreenView(frame: NSRect(x: 0, y: 0, width: 320, height: 240))
  // 첫 장이 오기 전에는 320x240 으로 둔다
  var w = 320, h = 240, seq = 0
  var got = false
  var sending: Task<Void, Never>?
  // 다른 앱을 쓰는 동안에도(항상 위) macOS 가 이 앱을 쉬게 하거나(App Nap) 화면 받기를 늦추지 않게 한다
  let awake = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .latencyCritical], reason: "기기 화면 미러링")

  override init() {
    let config = URLSessionConfiguration.ephemeral
    config.requestCachePolicy = .reloadIgnoringLocalCacheData
    config.timeoutIntervalForRequest = 10  // 서버는 1.5초 안에 답한다
    session = URLSession(configuration: config)
    super.init()
    defaults.register(defaults: ["onTop": true, "zoom": 1])
  }

  var zoom: Int { min(max(defaults.integer(forKey: "zoom"), 1), 3) }
  var onTop: Bool { defaults.bool(forKey: "onTop") }

  func applicationDidFinishLaunching(_ note: Notification) {
    NSApp.mainMenu = makeMainMenu()
    window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 320, height: 240), styleMask: [.titled, .closable, .miniaturizable],
                      backing: .buffered, defer: false)
    window.title = "LLM Deck 미러링"
    window.isReleasedWhenClosed = false
    window.contentView = view
    view.app = self
    window.center()
    window.setFrameAutosaveName("mirror")  // 지난번 창 위치를 기억한다
    fit()
    applyOnTop()
    window.makeKeyAndOrderFront(nil)
    window.makeFirstResponder(view)
    NSApp.activate()
    Task { await poll() }
  }

  func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { true }

  // 창 내용 크기를 기기 화면 × 배율로 맞춘다 (창 왼쪽 위 모서리는 그대로)
  func fit() {
    let size = NSSize(width: w * zoom, height: h * zoom)
    let now = window.frame
    if window.contentRect(forFrameRect: now).size == size { return }
    var frame = window.frameRect(forContentRect: NSRect(origin: .zero, size: size))
    frame.origin = NSPoint(x: now.minX, y: now.maxY - frame.height)
    window.setFrame(frame, display: true)
  }

  func applyOnTop() {
    window.level = onTop ? .floating : .normal
  }

  // 새 장을 계속 받아 간다. 이 요청이 이어지는 동안만 도우미가 기기에 화면을 보내 달라고 한다
  func poll() async {
    while true {
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
    menu.addItem(withTitle: "종료", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    return menu
  }

  func makeMainMenu() -> NSMenu {
    let bar = NSMenu()
    let top = NSMenuItem()
    top.submenu = makeMenu()
    top.submenu?.insertItem(withTitle: "창 닫기", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w", at: 0)
    top.submenu?.insertItem(.separator(), at: 1)
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

// 이미 떠 있으면 그 창을 앞으로 가져오고 끝낸다
let me = ProcessInfo.processInfo.processIdentifier
if let other = NSRunningApplication.runningApplications(withBundleIdentifier: BUNDLE_ID).first(where: { $0.processIdentifier != me }) {
  other.activate()
  exit(0)
}
MainActor.assumeIsolated {
  let app = NSApplication.shared
  let mirror = Mirror()
  app.delegate = mirror
  app.setActivationPolicy(.regular)
  app.run()
}
