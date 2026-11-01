// LLM Deck.app 의 실행 파일. install.sh 가 swiftc 로 컴파일한다.
// 블루투스·마이크 권한은 "앱" 단위로만 받을 수 있어서, 파이썬 도우미(main.py)를 이 앱의 자식 프로세스로 띄워 둔다.
// 메뉴 막대 아이콘에서 기기별 미러링 창을 연다 (S3 미러링, S31 미러링). 앱이 이미 떠 있을 때 한 번 더 열어도 (Finder, Spotlight, `uv run main.py --mirror`) S3 창만 연다.
// 미러링 창은 기기마다 따로 도우미가 띄운 http://127.0.0.1:4013/ (S3), :4015/ (S31) (mirror.py) 에서 화면을 받아 오고 누르기를 보낸다. 서버 쪽 규칙은 mirror.py 의 PAGE 와 같다.
// 창이 열려 있는 동안(화면을 받아 가는 동안)만 기기가 화면을 보낸다. 도우미(파이썬 자식 프로세스)는 S3 것만 이 앱이 띄운다. pppang31 은 launchd 서비스라 건드리지 않는다.
import AppKit

// 미러링 창을 여는 기기. 도우미(미러 서버)마다 포트가 다르다 (mirror.py 의 [mirror] port)
struct Device {
  let name: String
  let base: URL
  let suffix: String  // 저장 설정(UserDefaults) 키 뒤에 붙인다. S3 는 예전 키를 그대로 써서 쓰던 설정이 남는다
  let width: Int  // 첫 장이 오기 전에 잡는 화면 크기
  let height: Int
  let right: Double  // 처음 창 위치: 메인 화면 오른쪽 아래 모서리에서 떨어진 거리
  let bottom: Double

  static let s3 = Device(name: "S3", base: URL(string: "http://127.0.0.1:4013/")!, suffix: "", width: 320, height: 240, right: 12, bottom: 12)
  // S3 창(오른쪽 아래, 320 폭)의 왼쪽 옆에 둔다
  static let s31 = Device(name: "S31", base: URL(string: "http://127.0.0.1:4015/")!, suffix: "s31", width: 480, height: 480, right: 12 + 320 + 12, bottom: 12)
}

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

// ORCA 팝업 버튼의 표시명·입력 내용을 고치는 창 (기기에는 키보드가 없다). 서버 쪽 규칙은 popbuttons.py, 하는 일은 mirror.py 의 PAGE 와 같다.
// 미러링 창을 눌러도 가리지 않게 따로 띄우고, 화면 받기를 막지 않게 모달로 돌리지 않는다
@MainActor
final class EditPanel: NSObject, NSWindowDelegate {
  let session: URLSession
  let base: URL
  let deviceName: String
  let panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 360, height: 160), styleMask: [.titled, .closable], backing: .buffered, defer: false)
  let name = NSTextField(string: "")
  let text = NSTextField(string: "")
  let textTitle = NSTextField(labelWithString: "입력 내용 (띄어쓰기로 끝나면 Enter 없이 넣습니다)")
  let error = NSTextField(wrappingLabelWithString: "")
  let saveButton = NSButton(title: "저장", target: nil, action: nil)
  let cancelButton = NSButton(title: "취소", target: nil, action: nil)
  let stack = NSStackView()
  var seq = 0
  var id = ""

  init(session: URLSession, device: Device) {
    self.session = session
    base = device.base
    deviceName = device.name
    super.init()
    let width: CGFloat = 332
    panel.title = "\(device.name) 버튼 수정"
    panel.isReleasedWhenClosed = false
    panel.hidesOnDeactivate = false
    panel.level = .modalPanel
    panel.delegate = self
    error.textColor = .systemRed
    error.preferredMaxLayoutWidth = width
    saveButton.target = self
    saveButton.action = #selector(save)
    saveButton.keyEquivalent = "\r"
    cancelButton.target = self
    cancelButton.action = #selector(cancel)
    cancelButton.keyEquivalent = "\u{1b}"
    let spacer = NSView()
    spacer.setContentHuggingPriority(.init(1), for: .horizontal)
    let buttons = NSStackView(views: [spacer, cancelButton, saveButton])
    stack.orientation = .vertical
    stack.alignment = .leading
    stack.spacing = 6
    stack.edgeInsets = NSEdgeInsets(top: 12, left: 14, bottom: 12, right: 14)
    for view in [NSTextField(labelWithString: "표시명"), name, textTitle, text, error, buttons] { stack.addArrangedSubview(view) }
    for view in [name, text, error, buttons] { view.widthAnchor.constraint(equalToConstant: width).isActive = true }
    panel.contentView = stack
  }

  // 1초마다 읽은 요청. 요청이 사라지면 (다른 창에서 먼저 답함) 닫고, 새 요청이면 띄운다
  func update(_ req: [String: Any]) {
    guard let next = req["seq"] as? Int else {
      seq = 0
      panel.orderOut(nil)
      return
    }
    if next == seq { return }
    seq = next
    id = req["id"] as? String ?? ""
    panel.title = "\(deviceName) \(req["new"] as? Bool == true ? "새 버튼" : "버튼 수정")"
    name.stringValue = req["name"] as? String ?? ""
    text.stringValue = req["text"] as? String ?? ""
    let hasText = req["has_text"] as? Bool ?? false
    textTitle.isHidden = !hasText
    text.isHidden = !hasText
    say(nil)
    panel.center()
    panel.makeKeyAndOrderFront(nil)
    panel.makeFirstResponder(name)
    if #available(macOS 14, *) { NSApp.activate() } else { NSApp.activate(ignoringOtherApps: true) }
  }

  func say(_ message: String?) {
    error.stringValue = message ?? ""
    error.isHidden = message == nil
    stack.layoutSubtreeIfNeeded()
    panel.setContentSize(stack.fittingSize)
  }

  func request(_ body: [String: Any]) -> URLRequest {
    var req = URLRequest(url: URL(string: "edit", relativeTo: base)!)
    req.httpMethod = "POST"
    req.setValue("application/json", forHTTPHeaderField: "Content-Type")
    req.httpBody = try? JSONSerialization.data(withJSONObject: body)
    return req
  }

  @objc func save() {
    saveButton.isEnabled = false
    let req = request(["seq": seq, "id": id, "name": name.stringValue, "text": text.isHidden ? "" : text.stringValue])
    let asked = seq, session = session
    Task {
      var code = 0
      var data = Data()
      if let (body, res) = try? await session.data(for: req), let r = res as? HTTPURLResponse { (code, data) = (r.statusCode, body) }
      saveButton.isEnabled = true
      if asked != seq { return }  // 그 사이 다른 요청을 띄웠다
      if code == 200 || code == 409 {  // 409: 새 요청이 왔다. 곧 다시 읽는다
        panel.orderOut(nil)
      } else if code == 0 {
        say(OFFLINE)
      } else {
        let answer = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
        say(answer?["error"] as? String ?? "저장하지 못했습니다")
      }
    }
  }

  @objc func cancel() {
    guard panel.isVisible else { return }
    panel.orderOut(nil)
    let req = request(["seq": seq, "cancel": true]), session = session
    Task { _ = try? await session.data(for: req) }
  }

  func windowShouldClose(_ sender: NSWindow) -> Bool {
    cancel()
    return false  // 다시 띄울 창이라 숨기기만 한다
  }
}

@MainActor
final class Mirror: NSObject, NSWindowDelegate, NSMenuItemValidation {
  // 기기마다 하나씩 만든다. 세션, 화면 받기, 보내기, 저장 설정이 서로 섞이지 않는다
  let device: Device
  let defaults = UserDefaults.standard
  let session: URLSession
  let editor: EditPanel
  var window: NSWindow?
  let view: ScreenView
  // 첫 장이 오기 전에는 기기의 기본 크기로 둔다
  var w: Int, h: Int, seq = 0
  var got = false
  var sending: Task<Void, Never>?
  var polling: Task<Void, Never>?
  var editPolling: Task<Void, Never>?
  // 창이 열려 있는 동안은 다른 앱을 쓰고 있어도(항상 위) macOS 가 이 앱을 쉬게 하거나(App Nap) 화면 받기를 늦추지 않게 한다
  var awake: NSObjectProtocol?

  init(_ device: Device) {
    self.device = device
    (w, h) = (device.width, device.height)
    view = ScreenView(frame: NSRect(x: 0, y: 0, width: device.width, height: device.height))
    let config = URLSessionConfiguration.ephemeral
    config.requestCachePolicy = .reloadIgnoringLocalCacheData
    config.timeoutIntervalForRequest = 10  // 서버는 1.5초 안에 답한다
    session = URLSession(configuration: config)
    editor = EditPanel(session: session, device: device)
    super.init()
    // 예전 미러링 앱(LLM Deck Mirror.app)에 저장해 둔 배율·항상 위를 한 번 옮겨 온다 (S3 만 쓰던 앱이다)
    if device.suffix.isEmpty, defaults.object(forKey: "zoom") == nil, let old = UserDefaults(suiteName: "com.llmdeck.mirror") {
      for key in ["zoom", "onTop"] {
        if let v = old.object(forKey: key) { defaults.set(v, forKey: key) }
      }
    }
    defaults.register(defaults: [key("onTop"): true, key("zoom"): 1, key("mirrorRight"): device.right, key("mirrorBottom"): device.bottom])
  }

  // 저장 설정 키. S3 는 예전 이름 그대로, 다른 기기는 뒤에 기기 이름을 붙인다
  func key(_ name: String) -> String { device.suffix.isEmpty ? name : "\(name).\(device.suffix)" }

  // 배율. 메뉴의 1·2·3배, 또는 창 가장자리를 끌어 맞춘 크기 (0.5~4배). 화면보다 크면 fit() 이 줄여서 보여 준다
  var zoom: Double { min(max(defaults.double(forKey: key("zoom")), 0.5), 4) }
  var onTop: Bool { defaults.bool(forKey: key("onTop")) }

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
      editPolling = Task { await pollEdit() }
    }
  }

  func makeWindow() -> NSWindow {
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: device.width, height: device.height), styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
    window.title = "\(device.name) 미러링"
    window.isReleasedWhenClosed = false
    window.contentView = view
    window.delegate = self
    view.app = self
    place(window)
    self.window = window
    return window
  }

  // 메인 화면(메뉴 막대가 있는 화면)의 Dock·메뉴 막대를 뺀 영역. 모니터를 붙였다 떼도 이것을 기준으로 위치를 잡는다
  var mainArea: NSRect? { NSScreen.screens.first?.visibleFrame }

  // 지난번 닫은 위치에 창을 둔다 (창 오른쪽 아래 모서리 기준). 그 모니터가 지금 없으면 메인 화면 오른쪽 아래에서
  // 지난번만큼 떨어진 곳에 둔다 (처음에는 12pt). 화면 밖으로는 안 나가게
  func place(_ window: NSWindow) {
    let size = window.frame.size
    var corner: NSPoint
    var area: NSRect
    let last = defaults.string(forKey: key("mirrorFrame")).map(NSRectFromString)
    if let last, let screen = NSScreen.screens.first(where: { $0.visibleFrame.contains(NSPoint(x: last.midX, y: last.midY)) }) {
      corner = NSPoint(x: last.maxX, y: last.minY)
      area = screen.visibleFrame
    } else if let main = mainArea {
      corner = NSPoint(x: main.maxX - defaults.double(forKey: key("mirrorRight")), y: main.minY + defaults.double(forKey: key("mirrorBottom")))
      area = main
    } else { return }
    var origin = NSPoint(x: corner.x - size.width, y: corner.y)
    origin.x = min(max(origin.x, area.minX), area.maxX - size.width)
    origin.y = min(max(origin.y, area.minY), area.maxY - size.height)
    window.setFrameOrigin(origin)
  }

  // 창을 옮기면 위치를 기억한다 (닫을 때나 앱이 꺼질 때의 위치가 남는다). 메인 화면에 있으면 메인 화면 기준 위치도 기억한다
  func windowDidMove(_ note: Notification) {
    guard let window else { return }
    let frame = window.frame
    defaults.set(NSStringFromRect(frame), forKey: key("mirrorFrame"))
    if let main = mainArea, main.contains(NSPoint(x: frame.midX, y: frame.midY)) {
      defaults.set(main.maxX - frame.maxX, forKey: key("mirrorRight"))
      defaults.set(frame.minY - main.minY, forKey: key("mirrorBottom"))
    }
  }

  // 창을 닫으면 화면 받기를 멈춘다 (도우미가 기기에 미러링을 그만 보내게). 도우미는 계속 돈다
  func windowWillClose(_ note: Notification) {
    polling?.cancel()
    polling = nil
    editPolling?.cancel()
    editPolling = nil
    editor.cancel()
    if let awake { ProcessInfo.processInfo.endActivity(awake) }
    awake = nil
    // 다른 기기의 창이 남아 있으면 Dock 아이콘과 메뉴를 그대로 둔다
    let closing = note.object as? NSWindow
    let others = NSApp.windows.contains { $0 !== closing && $0.delegate is Mirror && ($0.isVisible || $0.isMiniaturized) }
    if !others { NSApp.setActivationPolicy(.accessory) }
  }

  // 창이 둘이면 메뉴 막대의 메뉴가 앞에 있는 창의 기기를 따라가게 한다
  func windowDidBecomeKey(_ note: Notification) {
    NSApp.mainMenu = makeMainMenu()
  }

  // 창 가장자리를 끌어 크기를 바꾸면 그 배율을 기억한다
  func windowDidEndLiveResize(_ note: Notification) {
    guard let window else { return }
    defaults.set(window.contentRect(forFrameRect: window.frame).width / CGFloat(w), forKey: key("zoom"))
    fit()
  }

  // 창 내용 크기를 기기 화면 × 배율로 맞춘다 (창 오른쪽 아래 모서리는 그대로, 화면 밖으로는 안 나가게)
  func fit() {
    guard let window, !window.inLiveResize else { return }  // 끄는 동안 새 장이 와도 되돌리지 않는다
    // 배율은 창(제목 줄 포함)이 화면에 들어오는 만큼까지만 (S31 은 480x480 이라 3배는 화면보다 크다). 저장한 배율은 그대로 둔다
    let area = (window.screen ?? NSScreen.main)?.visibleFrame
    let chrome = window.frameRect(forContentRect: .zero).size
    var limit = 4.0
    if let area { limit = min(limit, (area.width - chrome.width) / Double(w), (area.height - chrome.height) / Double(h)) }
    limit = max(limit, 0.5)
    // 끌어서 크기를 바꿀 때도 기기 화면 비율을 지킨다
    window.contentAspectRatio = NSSize(width: w, height: h)
    window.contentMinSize = NSSize(width: Double(w) * 0.5, height: Double(h) * 0.5)
    window.contentMaxSize = NSSize(width: Double(w) * limit, height: Double(h) * limit)
    let scale = min(zoom, limit)
    let size = NSSize(width: (Double(w) * scale).rounded(), height: (Double(h) * scale).rounded())
    let now = window.frame
    if window.contentRect(forFrameRect: now).size == size { return }
    var frame = window.frameRect(forContentRect: NSRect(origin: .zero, size: size))
    frame.origin = NSPoint(x: now.maxX - frame.width, y: now.minY)
    if let area {
      frame.origin.x = max(area.minX, frame.origin.x)
      frame.origin.y = min(frame.origin.y, area.maxY - frame.height)
    }
    window.setFrame(frame, display: true)
  }

  func applyOnTop() {
    window?.level = onTop ? .floating : .normal
  }

  // 새 장을 계속 받아 간다. 이 요청이 이어지는 동안만 도우미가 기기에 화면을 보내 달라고 한다
  func poll() async {
    while !Task.isCancelled {
      var req = URLRequest(url: URL(string: "frame?after=\(seq)", relativeTo: device.base)!)
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

  // ORCA 팝업 버튼 수정 요청을 1초마다 읽는다
  func pollEdit() async {
    while !Task.isCancelled {
      if let (data, _) = try? await session.data(for: URLRequest(url: URL(string: "edit", relativeTo: device.base)!)),
         let req = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] {
        editor.update(req)
      }
      try? await Task.sleep(for: .seconds(1))
    }
  }

  // 보낸 차례대로 도우미에 닿게 하나씩 보낸다
  func post(_ path: String, _ body: [String: Any]) {
    var req = URLRequest(url: URL(string: path, relativeTo: device.base)!)
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
    defaults.set(!onTop, forKey: key("onTop"))
    applyOnTop()
  }

  @objc func setZoom(_ item: NSMenuItem) {
    defaults.set(item.tag, forKey: key("zoom"))
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
    // 수정 창의 입력칸에서 붙여넣기 등을 쓰려면 메뉴에 있어야 한다
    let edit = NSMenu(title: "편집")
    edit.addItem(withTitle: "오려두기", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
    edit.addItem(withTitle: "복사하기", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
    edit.addItem(withTitle: "붙여넣기", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
    edit.addItem(withTitle: "모두 선택", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
    let editItem = NSMenuItem()
    editItem.submenu = edit
    bar.addItem(editItem)
    return bar
  }

  // 메뉴를 열 때마다 체크 표시를 지금 설정에 맞춘다
  func validateMenuItem(_ item: NSMenuItem) -> Bool {
    // 수정 창이 떠 있으면 Esc 는 그 창의 취소다 (기기가 홈으로 가지 않게)
    if item.action == #selector(home) { return !editor.panel.isVisible }
    if item.action == #selector(toggleOnTop) {
      item.state = onTop ? .on : .off
    } else if item.action == #selector(setZoom(_:)) {
      item.state = Double(item.tag) == zoom ? .on : .off
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
  let mirror = Mirror(.s3)  // 다시 열기, SIGUSR1 (S3 도우미의 자동 열기)는 S3 창만 연다
  let mirror31 = Mirror(.s31)
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
    add("S3 미러링", mirror, #selector(Mirror.open))
    add("S31 미러링", mirror31, #selector(Mirror.open))
    menu.addItem(.separator())
    add("S3 항상 위", mirror, #selector(Mirror.toggleOnTop))
    add("S31 항상 위", mirror31, #selector(Mirror.toggleOnTop))
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
