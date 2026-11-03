// LLM Deck.app 의 실행 파일. install.sh 가 swiftc 로 컴파일한다.
// 블루투스·마이크 권한은 "앱" 단위로만 받을 수 있어서, 파이썬 도우미(main.py)를 이 앱의 자식 프로세스로 띄워 둔다.
// 미러링 창은 하나고 그 안에 기기 칸을 하나만 보인다. 제목 줄의 전환 단추(S31, S3)나 메뉴 막대 아이콘(S31 미러링, S3 미러링)으로 기기를 바꾼다. 앱을 다시 열면 지금 칸을 앞으로 가져오고, 창이 닫혀 있으면 S31 을 먼저 연다 (S31 이 연결되어 있지 않으면 S3).
// 칸은 기기마다 따로 도우미가 띄운 http://127.0.0.1:4013/ (S3), :4015/ (S31) (mirror.py) 에서 화면을 받아 오고 누르기를 보낸다. 서버 쪽 규칙은 mirror.py 의 PAGE 와 같다.
// 칸이 열려 있는 동안(화면을 받아 가는 동안)만 기기가 화면을 보낸다. 도우미(파이썬 자식 프로세스)는 S3 것만 이 앱이 띄운다. pppang31 은 launchd 서비스라 건드리지 않는다.
import AppKit

// 미러링 창의 칸 하나가 보여 주는 기기. 도우미(미러 서버)마다 포트가 다르다 (mirror.py 의 [mirror] port)
struct Device {
  let name: String
  let base: URL
  let width: Int  // 첫 장이 오기 전에 잡는 화면 크기
  let height: Int
  let inches: Double  // 실물 화면의 대각선

  static let s3 = Device(name: "S3", base: URL(string: "http://127.0.0.1:4013/")!, width: 320, height: 240, inches: 2)
  static let s31 = Device(name: "S31", base: URL(string: "http://127.0.0.1:4015/")!, width: 480, height: 480, inches: 2.16)

  // 기기 화면 한 점이 창에서 차지하는 크기. 칸이 픽셀 수가 아니라 실물 크기 비율을 따른다 (배율 1 에서 1인치 = 200pt, S3 는 320x240 그대로)
  var pointsPerPixel: Double { 200 * inches / hypot(Double(width), Double(height)) }
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

  // 키보드(화살표, Esc)는 마지막에 누른 칸의 기기로 간다
  override func becomeFirstResponder() -> Bool {
    app?.focused(true)
    return true
  }

  override func resignFirstResponder() -> Bool {
    app?.focused(false)
    return true
  }

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

// 미러링 창의 기기 칸. 기기마다 하나씩 만든다. 세션, 화면 받기, 보내기가 서로 섞이지 않는다
@MainActor
final class Mirror: NSObject, NSMenuItemValidation {
  let device: Device
  let session: URLSession
  let editor: EditPanel
  let view: ScreenView
  let caption: NSTextField
  weak var shell: MirrorWindow?
  // 첫 장이 오기 전에는 기기의 기본 크기로 둔다
  var w: Int, h: Int, seq = 0
  var got = false
  // 기기의 나눠 쓰기 (도우미의 X-Split). nil 이면 모르거나 없는 기기라 합치기 단추를 숨긴다 (S31 은 늘 같은 화면)
  var split: Bool?
  var sending: Task<Void, Never>?
  var polling: Task<Void, Never>?
  var editPolling: Task<Void, Never>?
  // 칸이 열려 있는 동안은 다른 앱을 쓰고 있어도(항상 위) macOS 가 이 앱을 쉬게 하거나(App Nap) 화면 받기를 늦추지 않게 한다
  var awake: NSObjectProtocol?

  init(_ device: Device) {
    self.device = device
    (w, h) = (device.width, device.height)
    view = ScreenView(frame: NSRect(x: 0, y: 0, width: device.width, height: device.height))
    caption = NSTextField(labelWithString: device.name)
    caption.alignment = .center
    caption.font = .boldSystemFont(ofSize: 12)
    caption.textColor = .secondaryLabelColor
    let config = URLSessionConfiguration.ephemeral
    config.requestCachePolicy = .reloadIgnoringLocalCacheData
    config.timeoutIntervalForRequest = 10  // 서버는 1.5초 안에 답한다
    session = URLSession(configuration: config)
    editor = EditPanel(session: session, device: device)
    super.init()
    view.app = self
  }

  var isOpen: Bool { polling != nil }

  // 이 기기 칸으로 바꾼다 (창이 닫혀 있으면 창도). 메뉴 막대, 도우미의 자동 열기, 다시 열기가 부른다
  @objc func open() { shell?.show(self) }

  // 메뉴 막대: 다른 기기를 고르면 그 칸으로 바꾸고, 지금 칸을 고르면 창을 닫는다. 닫힌 칸의 기기는 화면을 보내지 않는다
  @objc func toggle() {
    if isOpen { shell?.hide(self) } else { shell?.show(self) }
  }

  // 도우미가 기기와 연결되어 있는지 묻는다 (화면을 받아 볼 수 있는 상태)
  func linked() async -> Bool {
    guard let (_, res) = try? await session.data(from: URL(string: "frame?after=\(seq)", relativeTo: device.base)!) else { return false }
    return [200, 204].contains((res as? HTTPURLResponse)?.statusCode)
  }

  func start() {
    guard polling == nil else { return }
    awake = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .latencyCritical], reason: "기기 화면 미러링")
    polling = Task { await poll() }
    editPolling = Task { await pollEdit() }
  }

  // 화면 받기를 멈춘다 (도우미가 기기에 미러링을 그만 보내게). 도우미는 계속 돈다
  func stop() {
    polling?.cancel()
    polling = nil
    editPolling?.cancel()
    editPolling = nil
    editor.cancel()
    if let awake { ProcessInfo.processInfo.endActivity(awake) }
    awake = nil
  }

  // 배율 1 일 때 칸의 크기
  var size: NSSize { NSSize(width: Double(w) * device.pointsPerPixel, height: Double(h) * device.pointsPerPixel) }

  func focused(_ on: Bool) {
    caption.textColor = on ? .labelColor : .secondaryLabelColor
    if on { shell?.focus = self }
  }

  // 새 장을 계속 받아 간다. 이 요청이 이어지는 동안만 도우미가 기기에 화면을 보내 달라고 한다
  func poll() async {
    while !Task.isCancelled {
      var req = URLRequest(url: URL(string: "frame?after=\(seq)", relativeTo: device.base)!)
      req.cachePolicy = .reloadIgnoringLocalCacheData
      do {
        let (data, res) = try await session.data(for: req)
        let r = res as! HTTPURLResponse
        let s = [200, 204].contains(r.statusCode) ? r.value(forHTTPHeaderField: "X-Split").map { $0 == "1" } : nil
        if s != split {
          split = s
          if isOpen { shell?.syncSplit() }
        }
        if r.statusCode == 200 {
          let fw = Int(r.value(forHTTPHeaderField: "X-Width") ?? "") ?? 0
          let fh = Int(r.value(forHTTPHeaderField: "X-Height") ?? "") ?? 0
          seq = Int(r.value(forHTTPHeaderField: "X-Seq") ?? "") ?? 0
          if fw > 0 && fh > 0, let image = unpack(data, fw, fh) {
            (w, h, got) = (fw, fh, true)
            view.image = image
            shell?.fit()
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

  // 합치기: 기기 LCD 와 미러링 창을 같은 화면으로. 분리: 기기 LCD 에는 사용량을 따로 (기기의 구독제 화면 단추와 같다)
  func toggleSplit() {
    guard let split else { return }
    post("split", ["on": !split])
  }

  // 눌린 칸의 오른쪽 메뉴
  func makeMenu() -> NSMenu { shell?.makeMenu(for: self) ?? NSMenu() }

  // 메뉴를 열 때마다 체크 표시를 지금 상태에 맞춘다
  func validateMenuItem(_ item: NSMenuItem) -> Bool {
    // 수정 창이 떠 있으면 Esc 는 그 창의 취소다 (어느 기기도 홈으로 가지 않게)
    if item.action == #selector(home) { return shell?.editing != true }
    if item.action == #selector(toggle) { item.state = isOpen ? .on : .off }
    return true
  }
}

// 기기 칸을 하나씩 보여 주는 미러링 창. 위치, 배율, 항상 위는 창 전체에 하나씩 둔다 (S3 만 쓰던 때의 저장 키를 그대로 쓴다)
@MainActor
final class MirrorWindow: NSObject, NSWindowDelegate, NSMenuItemValidation {
  static let captionHeight = 22.0
  static let minWidth = 160.0  // 제목 줄의 창 단추와 전환 단추가 들어가는 폭 (합치기 단추가 보이면 그만큼 더)
  let panes: [Mirror]
  let defaults = UserDefaults.standard
  var window: NSWindow?
  lazy var switcher = NSSegmentedControl(labels: panes.map(\.device.name), trackingMode: .selectOne, target: self, action: #selector(switchPanes(_:)))
  lazy var splitButton: NSButton = {
    let b = NSButton(title: "합치기", target: self, action: #selector(toggleSplit))
    b.controlSize = .small
    b.bezelStyle = .rounded
    b.isHidden = true
    return b
  }()
  // 제목 줄 오른쪽: [합치기] [S31 | S3]
  lazy var bar = NSStackView(views: [splitButton, switcher])
  var focus: Mirror? { didSet { if focus !== oldValue { NSApp.mainMenu = makeMainMenu() } } }

  init(_ panes: [Mirror]) {
    self.panes = panes
    // 예전 미러링 앱(LLM Deck Mirror.app)에 저장해 둔 배율·항상 위를 한 번 옮겨 온다
    if defaults.object(forKey: "zoom") == nil, let old = UserDefaults(suiteName: "com.llmdeck.mirror") {
      for key in ["zoom", "onTop"] {
        if let v = old.object(forKey: key) { defaults.set(v, forKey: key) }
      }
    }
    defaults.register(defaults: ["onTop": true, "zoom": 1, "mirrorRight": 12, "mirrorBottom": 12])
    super.init()
    for pane in panes { pane.shell = self }
  }

  // 배율. 메뉴의 1·2·3배, 또는 창 가장자리를 끌어 맞춘 크기 (0.5~4배). 화면보다 크면 fit() 이 줄여서 보여 준다
  var zoom: Double { min(max(defaults.double(forKey: "zoom"), 0.5), 4) }
  var onTop: Bool { defaults.bool(forKey: "onTop") }
  var open: [Mirror] { panes.filter(\.isOpen) }
  var editing: Bool { panes.contains { $0.editor.panel.isVisible } }

  // 이 칸으로 바꾼다 (다른 칸은 닫는다. 이미 이 칸이면 창을 앞으로). 창이 떠 있는 동안만 Dock 아이콘과 메뉴 막대 메뉴를 보인다
  func show(_ pane: Mirror) {
    let window = self.window ?? makeWindow()
    guard let content = window.contentView else { return }
    for other in open where other !== pane { remove(other) }
    pane.start()
    if pane.view.superview == nil {
      content.addSubview(pane.caption)
      content.addSubview(pane.view)
    }
    NSApp.setActivationPolicy(.regular)
    NSApp.mainMenu = makeMainMenu()
    syncSwitcher()
    syncSplit()
    applyOnTop()
    window.makeKeyAndOrderFront(nil)
    window.makeFirstResponder(pane.view)
    if #available(macOS 14, *) { NSApp.activate() } else { NSApp.activate(ignoringOtherApps: true) }
  }

  // 보이는 칸은 하나뿐이라 칸을 닫으면 창을 닫는다 (windowWillClose 가 칸을 정리한다)
  func hide(_ pane: Mirror) {
    if pane.isOpen { window?.performClose(nil) }
  }

  // 전환 단추: 고른 기기로 바꾼다. 다른 기기는 화면을 보내지 않는다
  @objc func switchPanes(_ control: NSSegmentedControl) {
    show(panes[control.selectedSegment])
  }

  func syncSwitcher() {
    switcher.selectedSegment = panes.firstIndex { $0.isOpen } ?? -1
  }

  // 합치기 단추: 지금 칸의 기기가 나눠 쓰기를 알려 줄 때만 보인다. 나눠 쓰는 중이면 "합치기", 같은 화면이면 "분리"
  func syncSplit() {
    let split = open.first?.split
    splitButton.isHidden = split == nil
    splitButton.title = split == false ? "분리" : "합치기"
    splitButton.toolTip = split == false ? "기기 화면에는 사용량을 따로 보여 줍니다" : "기기 화면과 미러링 창을 같은 화면으로 합칩니다"
    bar.setFrameSize(bar.fittingSize)
    fit()
  }

  @objc func toggleSplit() { open.first?.toggleSplit() }

  var minWidth: Double { MirrorWindow.minWidth + (splitButton.isHidden ? 0 : splitButton.fittingSize.width + bar.spacing) }

  func remove(_ pane: Mirror) {
    pane.stop()
    pane.view.removeFromSuperview()
    pane.caption.removeFromSuperview()
    pane.focused(false)
    if focus === pane { focus = nil }
  }

  func makeWindow() -> NSWindow {
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 320, height: 240), styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
    window.title = "LLM Deck 미러링"
    window.titleVisibility = .hidden  // 좁은 창에서 글자가 전환 단추와 겹치지 않게. 기기 이름은 칸 위에 있다
    window.isReleasedWhenClosed = false
    window.backgroundColor = NSColor(red: 0.04, green: 0.05, blue: 0.07, alpha: 1)
    window.delegate = self
    switcher.controlSize = .small
    switcher.sizeToFit()
    bar.spacing = 6
    bar.setFrameSize(bar.fittingSize)
    let accessory = NSTitlebarAccessoryViewController()
    accessory.view = bar
    accessory.layoutAttribute = .trailing
    window.addTitlebarAccessoryViewController(accessory)
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
    let last = defaults.string(forKey: "mirrorFrame").map(NSRectFromString)
    if let last, let screen = NSScreen.screens.first(where: { $0.visibleFrame.contains(NSPoint(x: last.midX, y: last.midY)) }) {
      corner = NSPoint(x: last.maxX, y: last.minY)
      area = screen.visibleFrame
    } else if let main = mainArea {
      corner = NSPoint(x: main.maxX - defaults.double(forKey: "mirrorRight"), y: main.minY + defaults.double(forKey: "mirrorBottom"))
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
    defaults.set(NSStringFromRect(frame), forKey: "mirrorFrame")
    if let main = mainArea, main.contains(NSPoint(x: frame.midX, y: frame.midY)) {
      defaults.set(main.maxX - frame.maxX, forKey: "mirrorRight")
      defaults.set(frame.minY - main.minY, forKey: "mirrorBottom")
    }
  }

  // 창을 닫으면 모든 칸의 화면 받기를 멈춘다. 도우미는 계속 돈다
  func windowWillClose(_ note: Notification) {
    for pane in open { remove(pane) }
    NSApp.setActivationPolicy(.accessory)
  }

  // 배율 1 일 때 칸의 크기
  var natural: NSSize { open.first?.size ?? .zero }

  // 배율에 맞는 창 내용 크기
  func contentSize(_ scale: Double) -> NSSize {
    NSSize(width: (natural.width * scale).rounded(), height: (natural.height * scale + MirrorWindow.captionHeight).rounded())
  }

  // 지금 창 내용 크기에 들어가는 가장 큰 배율
  func scale(in size: NSSize) -> Double {
    guard natural.width > 0, natural.height > 0 else { return 1 }
    return min(size.width / natural.width, (size.height - MirrorWindow.captionHeight) / natural.height)
  }

  // 칸을 창 가운데 놓는다. 창 내용의 좌표는 아래에서 위로 잰다
  func layoutPanes() {
    guard let content = window?.contentView, let pane = open.first else { return }
    let size = contentSize(scale(in: content.bounds.size))
    let width = size.width, height = size.height - MirrorWindow.captionHeight
    let x = ((content.bounds.width - width) / 2).rounded()
    let top = content.bounds.height - MirrorWindow.captionHeight
    pane.caption.frame = NSRect(x: x, y: top + 2, width: width, height: MirrorWindow.captionHeight - 4)
    pane.view.frame = NSRect(x: x, y: top - height, width: width, height: height)
  }

  func windowDidResize(_ note: Notification) { layoutPanes() }

  // 창 가장자리를 끌어 크기를 바꾸면 그 배율을 기억한다
  func windowDidEndLiveResize(_ note: Notification) {
    guard let window else { return }
    defaults.set(scale(in: window.contentRect(forFrameRect: window.frame).size), forKey: "zoom")
    fit()
  }

  // 창 내용 크기를 칸의 크기 × 배율로 맞춘다 (창 오른쪽 아래 모서리는 그대로, 화면 밖으로는 안 나가게)
  func fit() {
    guard let window, !window.inLiveResize else { return }  // 끄는 동안 새 장이 와도 되돌리지 않는다
    // 배율은 창(제목 줄 포함)이 화면에 들어오는 만큼까지만. 저장한 배율은 그대로 둔다
    let area = (window.screen ?? NSScreen.main)?.visibleFrame
    let chrome = window.frameRect(forContentRect: .zero).size
    var limit = 4.0
    if let area { limit = min(limit, scale(in: NSSize(width: area.width - chrome.width, height: area.height - chrome.height))) }
    limit = max(limit, 0.5)
    func wide(_ size: NSSize) -> NSSize { NSSize(width: max(size.width, minWidth), height: size.height) }
    window.contentMinSize = wide(contentSize(0.5))
    window.contentMaxSize = wide(contentSize(limit))
    let size = wide(contentSize(min(zoom, limit)))
    let now = window.frame
    if window.contentRect(forFrameRect: now).size == size {
      layoutPanes()  // 창 크기는 그대로인데 기기 화면 크기가 바뀌었을 수 있다
      return
    }
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

  @objc func toggleOnTop() {
    defaults.set(!onTop, forKey: "onTop")
    applyOnTop()
  }

  @objc func setZoom(_ item: NSMenuItem) {
    defaults.set(item.tag, forKey: "zoom")
    fit()
  }

  // 메뉴 막대와 오른쪽 메뉴에 같이 넣는 항목들. 홈은 그 기기로 간다
  func items(for pane: Mirror?) -> [NSMenuItem] {
    func item(_ title: String, _ action: Selector, _ key: String, tag: Int = 0) -> NSMenuItem {
      let it = NSMenuItem(title: title, action: action, keyEquivalent: key)
      it.target = self
      it.tag = tag
      return it
    }
    var list = [
      item("항상 위", #selector(toggleOnTop), "t"),
      .separator(),
      item("1배", #selector(setZoom(_:)), "1", tag: 1),
      item("2배", #selector(setZoom(_:)), "2", tag: 2),
      item("3배", #selector(setZoom(_:)), "3", tag: 3),
    ]
    if let pane {
      let esc = NSMenuItem(title: "\(pane.device.name) 홈 (Esc)", action: #selector(Mirror.home), keyEquivalent: "\u{1b}")
      esc.target = pane
      esc.keyEquivalentModifierMask = []
      list += [.separator(), esc]
    }
    return list
  }

  func makeMenu(for pane: Mirror?) -> NSMenu {
    let menu = NSMenu()
    for it in items(for: pane) { menu.addItem(it) }
    menu.addItem(.separator())
    // 종료는 두지 않는다 (도우미까지 꺼진다). 창을 닫아도 도우미는 메뉴 막대에 남는다
    menu.addItem(withTitle: "미러링 닫기", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
    return menu
  }

  func makeMainMenu() -> NSMenu {
    let bar = NSMenu()
    let top = NSMenuItem()
    top.submenu = makeMenu(for: focus)
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
  let mirror = Mirror(.s3)
  let mirror31 = Mirror(.s31)  // SIGUSR2 (S31 도우미의 자동 열기)
  lazy var shell = MirrorWindow([mirror31, mirror])
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
    // 도우미가 기기와 새로 연결되면 신호로 창을 열게 한다. S31 이 우선이라 S31 이 연결되면 그 칸으로 바꾸고, S3 는 창이 닫혀 있을 때만 연다
    let handlers: [(Int32, @MainActor () -> Void)] = [
      (SIGUSR1, { [unowned self] in if self.shell.open.isEmpty { self.openDefault() } }),
      (SIGUSR2, { [unowned self] in self.mirror31.open() }),
    ]
    for (sig, handler) in handlers {
      signal(sig, SIG_IGN)
      let open = DispatchSource.makeSignalSource(signal: sig, queue: .main)
      open.setEventHandler { MainActor.assumeIsolated { handler() } }
      open.resume()
      signals.append(open)
    }
    let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    item.button?.image = NSImage(systemSymbolName: "rectangle.on.rectangle", accessibilityDescription: "LLM Deck")
    item.button?.toolTip = "LLM Deck"
    let menu = NSMenu()
    func add(_ title: String, _ target: AnyObject, _ action: Selector) {
      let it = menu.addItem(withTitle: title, action: action, keyEquivalent: "")
      it.target = target
    }
    add("S31 미러링", mirror31, #selector(Mirror.toggle))
    add("S3 미러링", mirror, #selector(Mirror.toggle))
    menu.addItem(.separator())
    add("항상 위", shell, #selector(MirrorWindow.toggleOnTop))
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

  // 지금 칸을 앞으로 가져온다. 창이 닫혀 있으면 S31 을 먼저 열고, S31 이 연결되어 있지 않고 S3 는 연결되어 있으면 S3 를 연다
  func openDefault() {
    if let pane = shell.open.first {
      pane.open()
      return
    }
    Task {
      let s31 = await mirror31.linked()
      let s3 = s31 ? false : await mirror.linked()
      if shell.open.isEmpty { (s3 ? mirror : mirror31).open() }
    }
  }

  // 이미 떠 있는 앱을 한 번 더 열면 미러링 창을 연다
  func applicationShouldHandleReopen(_ app: NSApplication, hasVisibleWindows: Bool) -> Bool {
    openDefault()
    return false
  }

  func applicationWillTerminate(_ note: Notification) { helper.stop() }

  @objc func showLog() { NSWorkspace.shared.open(LOG) }
}

MainActor.assumeIsolated {
  signal(SIGUSR2, SIG_IGN)  // S31 도우미는 이 앱의 자식이 아니라 앱이 막 켜진 참에도 보낼 수 있다. 받을 준비 전에 앱이 꺼지지 않게
  let app = NSApplication.shared
  let delegate = App()
  app.delegate = delegate
  app.setActivationPolicy(.accessory)
  app.run()
}
