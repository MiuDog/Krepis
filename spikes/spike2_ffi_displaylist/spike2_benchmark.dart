import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';
import 'dart:convert';

// C ABI 型別定義
typedef KrepisEngineHandle = Pointer<Void>;
typedef KrepisDisplayListHandle = Pointer<Void>;

typedef KrepisEngineCreateC = Int32 Function(Pointer<KrepisEngineHandle> outEngine);
typedef KrepisEngineCreateDart = int Function(Pointer<KrepisEngineHandle> outEngine);

typedef KrepisEngineDestroyC = Int32 Function(KrepisEngineHandle engine);
typedef KrepisEngineDestroyDart = int Function(KrepisEngineHandle engine);

typedef KrepisEngineSetViewportC = Int32 Function(
    KrepisEngineHandle engine, Float width, Float height, Float scrollY);
typedef KrepisEngineSetViewportDart = int Function(
    KrepisEngineHandle engine, double width, double height, double scrollY);

typedef KrepisEngineInsertParagraphC = Int32 Function(
    KrepisEngineHandle engine, Uint32 index, Pointer<Uint8> utf8Text);
typedef KrepisEngineInsertParagraphDart = int Function(
    KrepisEngineHandle engine, int index, Pointer<Uint8> utf8Text);

typedef KrepisEngineEditParagraphC = Int32 Function(
    KrepisEngineHandle engine, Uint32 index, Pointer<Uint8> utf8Text);
typedef KrepisEngineEditParagraphDart = int Function(
    KrepisEngineHandle engine, int index, Pointer<Uint8> utf8Text);

typedef KrepisEngineLayoutC = Int32 Function(
    KrepisEngineHandle engine, Pointer<Float> outTotalHeight);
typedef KrepisEngineLayoutDart = int Function(
    KrepisEngineHandle engine, Pointer<Float> outTotalHeight);

typedef KrepisEngineAcquireDisplayListC = Int32 Function(
    KrepisEngineHandle engine,
    Pointer<Pointer<Uint8>> outBufferPtr,
    Pointer<Uint32> outBufferSize,
    Pointer<KrepisDisplayListHandle> outDlHandle);
typedef KrepisEngineAcquireDisplayListDart = int Function(
    KrepisEngineHandle engine,
    Pointer<Pointer<Uint8>> outBufferPtr,
    Pointer<Uint32> outBufferSize,
    Pointer<KrepisDisplayListHandle> outDlHandle);

typedef KrepisDisplayListReleaseC = Int32 Function(
    KrepisEngineHandle engine, KrepisDisplayListHandle dlHandle);
typedef KrepisDisplayListReleaseDart = int Function(
    KrepisEngineHandle engine, KrepisDisplayListHandle dlHandle);

// 本地記憶體配置輔助（使用 Windows msvcrt malloc / free）
class NativeMemory {
  static final DynamicLibrary _stdlib = DynamicLibrary.process();
  static final Pointer<Void> Function(int) _malloc = _stdlib
      .lookup<NativeFunction<Pointer<Void> Function(IntPtr)>>('malloc')
      .asFunction();
  static final void Function(Pointer<Void>) _free = _stdlib
      .lookup<NativeFunction<Void Function(Pointer<Void>)>>('free')
      .asFunction();

  static Pointer<T> alloc<T extends NativeType>(int bytes) {
    final ptr = _malloc(bytes);
    if (ptr == nullptr) throw OutOfMemoryError();
    return ptr.cast<T>();
  }

  static void free(Pointer ptr) {
    if (ptr != nullptr) {
      _free(ptr.cast<Void>());
    }
  }

  static Pointer<Uint8> stringToUtf8(String str) {
    final units = utf8.encode(str);
    final ptr = alloc<Uint8>(units.length + 1);
    final list = ptr.asTypedList(units.length + 1);
    list.setAll(0, units);
    list[units.length] = 0; // null-terminated
    return ptr;
  }
}

class KrepisBindings {
  late DynamicLibrary _dylib;
  late KrepisEngineCreateDart createEngine;
  late KrepisEngineDestroyDart destroyEngine;
  late KrepisEngineSetViewportDart setViewport;
  late KrepisEngineInsertParagraphDart insertParagraph;
  late KrepisEngineEditParagraphDart editParagraph;
  late KrepisEngineLayoutDart layout;
  late KrepisEngineAcquireDisplayListDart acquireDisplayList;
  late KrepisDisplayListReleaseDart releaseDisplayList;

  KrepisBindings(String dllPath) {
    _dylib = DynamicLibrary.open(dllPath);
    createEngine = _dylib
        .lookupFunction<KrepisEngineCreateC, KrepisEngineCreateDart>('krepis_engine_create');
    destroyEngine = _dylib
        .lookupFunction<KrepisEngineDestroyC, KrepisEngineDestroyDart>('krepis_engine_destroy');
    setViewport = _dylib
        .lookupFunction<KrepisEngineSetViewportC, KrepisEngineSetViewportDart>('krepis_engine_set_viewport');
    insertParagraph = _dylib
        .lookupFunction<KrepisEngineInsertParagraphC, KrepisEngineInsertParagraphDart>('krepis_engine_insert_paragraph');
    editParagraph = _dylib
        .lookupFunction<KrepisEngineEditParagraphC, KrepisEngineEditParagraphDart>('krepis_engine_edit_paragraph');
    layout = _dylib
        .lookupFunction<KrepisEngineLayoutC, KrepisEngineLayoutDart>('krepis_engine_layout');
    acquireDisplayList = _dylib
        .lookupFunction<KrepisEngineAcquireDisplayListC, KrepisEngineAcquireDisplayListDart>('krepis_engine_acquire_display_list');
    releaseDisplayList = _dylib
        .lookupFunction<KrepisDisplayListReleaseC, KrepisDisplayListReleaseDart>('krepis_display_list_release');
  }
}

// 模擬 Dart 側對 Display List 二進位緩衝區進行零複製解碼與繪製遍歷
int processDisplayListZeroCopy(Pointer<Uint8> bufferPtr, int bufferSize) {
  if (bufferPtr == nullptr || bufferSize < 32) return 0;
  
  // 零複製 TypedData View
  final uint8View = bufferPtr.asTypedList(bufferSize);
  final byteData = ByteData.view(uint8View.buffer, uint8View.offsetInBytes, bufferSize);

  final magic = byteData.getUint32(0, Endian.little);
  final version = byteData.getUint32(4, Endian.little);
  final totalBytes = byteData.getUint32(8, Endian.little);
  final commandCount = byteData.getUint32(12, Endian.little);

  assert(magic == 0x4B524550, 'Magic mismatch');
  assert(version == 1, 'Version mismatch');

  int offset = 32; // Header 大小
  int processedCmds = 0;

  for (int i = 0; i < commandCount && offset < totalBytes; ++i) {
    final cmdType = byteData.getUint32(offset, Endian.little);
    if (cmdType == 1) {
      // DrawRect: 24 bytes
      offset += 24;
      processedCmds++;
    } else if (cmdType == 2) {
      // DrawGlyphRun
      final glyphCount = byteData.getUint32(offset + 20, Endian.little);
      final headerSize = 24;
      final indicesSize = glyphCount * 2;
      final indicesAlignedSize = (indicesSize + 3) & ~3;
      final advancesSize = glyphCount * 4;
      offset += headerSize + indicesAlignedSize + advancesSize;
      processedCmds++;
    } else {
      break;
    }
  }

  return processedCmds;
}

void main() {
  final dllPath = 'C:\\Projects\\Krepis\\build\\msvc-x64\\spikes\\Debug\\krepis_c_abi.dll';
  if (!File(dllPath).existsSync()) {
    print('找不到 DLL: $dllPath');
    exit(1);
  }

  print('===============================================================');
  print('[Spike 2] Dart FFI ↔ C++ 120Hz 捲動吞吐與增量排版基準測試');
  print('===============================================================');

  final bindings = KrepisBindings(dllPath);

  // 測試矩陣：段落數 N = 100, 1,000, 10,000
  final paragraphScales = [100, 1000, 10000];

  for (final n in paragraphScales) {
    print('\n---------------------------------------------------------------');
    print('▶ 測試情境：文件大小 N = $n 段落');
    print('---------------------------------------------------------------');

    final enginePtr = NativeMemory.alloc<KrepisEngineHandle>(sizeOf<KrepisEngineHandle>());
    bindings.createEngine(enginePtr);
    final engine = enginePtr.value;

    // 初始化視窗 800x600
    bindings.setViewport(engine, 800.0, 600.0, 0.0);

    // 填充 N 個段落
    for (int i = 0; i < n; ++i) {
      final text = '段落 #$i：Krepis 結構化筆記 120Hz 增量版面引擎實測。多語言混排 Segoe UI 測試文字。';
      final textNative = NativeMemory.stringToUtf8(text);
      bindings.insertParagraph(engine, i, textNative);
      NativeMemory.free(textNative);
    }

    // 初次完整排版
    final totalHeightPtr = NativeMemory.alloc<Float>(sizeOf<Float>());
    bindings.layout(engine, totalHeightPtr);
    final totalHeight = totalHeightPtr.value;
    print('已載入 $n 段落，文件總高度: ${totalHeight.toStringAsFixed(1)} px');

    // -------------------------------------------------------------
    // 基準 1：120Hz 持續捲動模擬 (1,000 幀)
    // -------------------------------------------------------------
    final scrollFrames = 1000;
    final scrollStep = 5.0; // 每幀捲動 5px
    final scrollLatenciesUs = <double>[];

    final outBufPtr = NativeMemory.alloc<Pointer<Uint8>>(sizeOf<Pointer<Uint8>>());
    final outBufSize = NativeMemory.alloc<Uint32>(sizeOf<Uint32>());
    final outDlHandle = NativeMemory.alloc<KrepisDisplayListHandle>(sizeOf<KrepisDisplayListHandle>());

    for (int f = 0; f < scrollFrames; ++f) {
      final scrollY = (f * scrollStep) % (totalHeight > 600 ? totalHeight - 600 : 100);

      final sw = Stopwatch()..start();

      // 1. 設定 Viewport 捲動位移
      bindings.setViewport(engine, 800.0, 600.0, scrollY);

      // 2. 觸發 Layout (由於無文字髒標記，純座標計算或跳過)
      bindings.layout(engine, nullptr);

      // 3. 取得 Display List
      bindings.acquireDisplayList(engine, outBufPtr, outBufSize, outDlHandle);

      // 4. Dart 零複製解析 Display List
      final cmdCount = processDisplayListZeroCopy(outBufPtr.value, outBufSize.value);

      // 5. 釋放 Display List Handle
      bindings.releaseDisplayList(engine, outDlHandle.value);

      sw.stop();
      scrollLatenciesUs.add(sw.elapsedMicroseconds.toDouble());
    }

    scrollLatenciesUs.sort();
    final scrollP50 = scrollLatenciesUs[(scrollFrames * 0.50).toInt()];
    final scrollP90 = scrollLatenciesUs[(scrollFrames * 0.90).toInt()];
    final scrollP99 = scrollLatenciesUs[(scrollFrames * 0.99).toInt()];
    final scrollMax = scrollLatenciesUs.last;
    final scrollAvg = scrollLatenciesUs.reduce((a, b) => a + b) / scrollFrames;

    print('[1. 捲動持續表現 (120Hz 每幀往返)]');
    print('  - 平均: ${scrollAvg.toStringAsFixed(1)} μs (${(scrollAvg / 1000).toStringAsFixed(3)} ms)');
    print('  - p50:  ${scrollP50.toStringAsFixed(1)} μs (${(scrollP50 / 1000).toStringAsFixed(3)} ms)');
    print('  - p90:  ${scrollP90.toStringAsFixed(1)} μs (${(scrollP90 / 1000).toStringAsFixed(3)} ms)');
    print('  - p99:  ${scrollP99.toStringAsFixed(1)} μs (${(scrollP99 / 1000).toStringAsFixed(3)} ms)');
    print('  - max:  ${scrollMax.toStringAsFixed(1)} μs (${(scrollMax / 1000).toStringAsFixed(3)} ms)');
    print('  - 120Hz 預算 (8.33ms) 佔比 (p99): ${((scrollP99 / 8333.0) * 100).toStringAsFixed(2)}%');

    // -------------------------------------------------------------
    // 基準 2：單字打字增量編輯 (100 次打字按鍵)
    // -------------------------------------------------------------
    final typingRuns = 100;
    final typingLatenciesUs = <double>[];
    final targetIndex = (n / 2).toInt(); // 編輯中間段落

    for (int t = 0; t < typingRuns; ++t) {
      final editedText = '段落 #$targetIndex：Krepis 結構化筆記 120Hz 增量版面引擎實測。使用者正在打字 [鍵入字元 $t]...';
      final textNative = NativeMemory.stringToUtf8(editedText);

      final sw = Stopwatch()..start();

      // 1. 編輯段落
      bindings.editParagraph(engine, targetIndex, textNative);

      // 2. 增量排版
      bindings.layout(engine, nullptr);

      // 3. 取得 Display List
      bindings.acquireDisplayList(engine, outBufPtr, outBufSize, outDlHandle);

      // 4. Dart 零複製讀取
      processDisplayListZeroCopy(outBufPtr.value, outBufSize.value);

      // 5. 釋放
      bindings.releaseDisplayList(engine, outDlHandle.value);

      sw.stop();
      typingLatenciesUs.add(sw.elapsedMicroseconds.toDouble());

      NativeMemory.free(textNative);
    }

    typingLatenciesUs.sort();
    final typingP50 = typingLatenciesUs[(typingRuns * 0.50).toInt()];
    final typingP90 = typingLatenciesUs[(typingRuns * 0.90).toInt()];
    final typingP99 = typingLatenciesUs[(typingRuns * 0.99).toInt()];
    final typingMax = typingLatenciesUs.last;
    final typingAvg = typingLatenciesUs.reduce((a, b) => a + b) / typingRuns;

    print('[2. 打字增量編輯往返表現 (包含 Shaping + Layout + FFI + Zero-copy)]');
    print('  - 平均: ${typingAvg.toStringAsFixed(1)} μs (${(typingAvg / 1000).toStringAsFixed(3)} ms)');
    print('  - p50:  ${typingP50.toStringAsFixed(1)} μs (${(typingP50 / 1000).toStringAsFixed(3)} ms)');
    print('  - p90:  ${typingP90.toStringAsFixed(1)} μs (${(typingP90 / 1000).toStringAsFixed(3)} ms)');
    print('  - p99:  ${typingP99.toStringAsFixed(1)} μs (${(typingP99 / 1000).toStringAsFixed(3)} ms)');
    print('  - max:  ${typingMax.toStringAsFixed(1)} μs (${(typingMax / 1000).toStringAsFixed(3)} ms)');

    // 釋放記憶體
    NativeMemory.free(outBufPtr);
    NativeMemory.free(outBufSize);
    NativeMemory.free(outDlHandle);
    NativeMemory.free(totalHeightPtr);
    bindings.destroyEngine(engine);
    NativeMemory.free(enginePtr);
  }

  print('\n===============================================================');
  print('--> [通過] Spike 2 全部測試完成！');
  print('===============================================================');
}
