let loadPromise = null;

function loadQrLibrary() {
  if (typeof globalThis.QRCode !== "undefined") {
    return Promise.resolve(globalThis.QRCode);
  }
  if (loadPromise) {
    return loadPromise;
  }
  loadPromise = new Promise((resolve, reject) => {
    const script = document.createElement("script");
    script.src = "/common/qrcode.bundle.js";
    script.async = true;
    script.onload = () => {
      if (typeof globalThis.QRCode === "undefined") {
        reject(new Error("二维码库加载失败"));
        return;
      }
      resolve(globalThis.QRCode);
    };
    script.onerror = () => reject(new Error("二维码库加载失败"));
    document.head.appendChild(script);
  });
  return loadPromise;
}

export async function renderQrCode(container, text) {
  if (!text) {
    throw new Error("二维码内容为空");
  }

  const QRCode = await loadQrLibrary();
  const canvas = document.createElement("canvas");

  await new Promise((resolve, reject) => {
    QRCode.toCanvas(
      canvas,
      text,
      { width: 240, margin: 2, errorCorrectionLevel: "M" },
      (error) => {
        if (error) {
          reject(error);
          return;
        }
        resolve();
      }
    );
  });

  container.replaceChildren();
  const title = document.createElement("p");
  title.textContent = "请使用手机扫码进入支付页";
  const hint = document.createElement("code");
  hint.textContent = text;
  container.append(title, canvas, hint);
}
