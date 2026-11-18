// Finestra di rendering (comando Render): immagine progressiva del path tracer,
// con salvataggio in PNG/JPG come la finestra di rendering di Rhino.
#pragma once
#include "render/Render.h"
#include <QWidget>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

class QLabel;
class QPushButton;

namespace cad::ui {

class Scene;

class RenderWindow : public QWidget {
public:
  RenderWindow(Engine& e, const Scene& scene, const ON_3dmView& view, int w, int h, int samples, QWidget* parent = nullptr);
  ~RenderWindow() override;

protected:
  void closeEvent(QCloseEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;

private:
  void Stop();
  void Show();
  std::shared_ptr<RenderScene> m_scene;
  std::unique_ptr<PathTracer> m_pt;
  std::thread m_thread;
  std::atomic<bool> m_cancel{false};
  std::mutex m_mx;
  std::vector<unsigned char> m_img;
  int m_w = 0, m_h = 0, m_samples = 0, m_passes = 0;
  QLabel* m_view = nullptr;
  QLabel* m_status = nullptr;
  QPushButton* m_stop = nullptr;
};

}  // namespace cad::ui
