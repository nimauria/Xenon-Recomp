// Deliberately independent of React and the module bundle: startup errors stay visible.
(() => {
  const show = (message) => {
    let panel = document.getElementById("startup-error");
    if (!panel) {
      panel = document.createElement("section");
      panel.id = "startup-error";
      panel.setAttribute("role", "alert");
      Object.assign(panel.style, {
        position: "fixed",
        inset: "20px",
        zIndex: "9999",
        padding: "32px",
        overflow: "auto",
        background: "#07131d",
        color: "#f4f8fa",
        font: "16px system-ui",
        border: "1px solid #f06470",
      });
      const title = document.createElement("h1");
      title.textContent = "Xenon could not open the interface";
      panel.append(title);
      const detail = document.createElement("pre");
      detail.id = "startup-detail";
      detail.style.whiteSpace = "pre-wrap";
      panel.append(detail);
      const hint = document.createElement("p");
      hint.textContent =
        "Your launcher data has been preserved. Restart the launcher after rebuilding, or reload to try again.";
      panel.append(hint);
      const reload = document.createElement("button");
      reload.textContent = "Reload launcher";
      reload.onclick = () => location.reload();
      panel.append(reload);
      document.body.append(panel);
    }
    document.getElementById("startup-detail").textContent = String(message);
  };
  window.addEventListener(
    "error",
    (event) => {
      if (event.target instanceof HTMLScriptElement)
        show(`Could not load the application script: ${event.target.src}`);
      else if (event.message) show(event.message);
    },
    true,
  );
  window.addEventListener("unhandledrejection", (event) =>
    show(
      event.reason?.message ||
        event.reason ||
        "An unexpected startup error occurred.",
    ),
  );
  setTimeout(() => {
    if (document.querySelector("[data-startup]"))
      show(
        "The application bundle did not start within 15 seconds. Check the packaged web assets and startup log.",
      );
  }, 15000);
})();
