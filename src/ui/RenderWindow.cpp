#include "ui/RenderWindow.h"
#include "ui/Scene.h"
#include <QCloseEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>
#include <chrono>

namespace cad::ui {

RenderWindow::RenderWindow(Engine& e, const Scene& scene, const ON_3dmView& view, int w, int h, int samples, QWidget* parent)
    : QWidget(parent, Qt::Window), m_w(w), m_h(h), m_samples(samples) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle("Rendering - Kurva3D");
  auto* v = new QVBoxLayout(this);
  m_view = new QLabel;
  m_view->setAlignment(Qt::AlignCenter);
  m_view->setMinimumSize(320, 240);
  m_view->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  v->addWidget(m_view, 1);
  auto* row = new QHBoxLayout;
  m_status = new QLabel;
  row->addWidget(m_status, 1);
  m_stop = new QPushButton("Ferma");
  auto* save = new QPushButton("Salva immagine...");
  row->addWidget(m_stop);
  row->addWidget(save);
  v->addLayout(row);
  connect(m_stop, &QPushButton::clicked, this, [this] { Stop(); });
  connect(save, &QPushButton::clicked, this, [this] {
    QImage img;
    {
      std::lock_guard<std::mutex> lk(m_mx);
      if (m_img.empty()) return;
      img = QImage(m_img.data(), m_w, m_h, m_w * 3, QImage::Format_RGB888).copy();
    }
    const QString p = QFileDialog::getSaveFileName(this, "Salva rendering", "render.png", "PNG (*.png);;JPEG (*.jpg);;BMP (*.bmp)");
    if (!p.isEmpty() && !img.save(p)) QMessageBox::warning(this, "Rendering", "Impossibile salvare " + p);
  });
  resize(std::min(w, 1400) + 20, std::min(h, 900) + 60);

  RenderOptions ro;
  ro.width = w;
  ro.height = h;
  ro.samples = samples;
  m_scene = std::make_shared<RenderScene>();
  m_scene->Build(e, scene, ro);
  RenderCamera cam;
  cam.Init(view, double(w) / h);
  m_pt = std::make_unique<PathTracer>(*m_scene, cam, ro);
  m_status->setText(QString("%1 x %2, %3 triangoli").arg(w).arg(h).arg(m_scene->TriangleCount()));
  const auto t0 = std::chrono::steady_clock::now();
  m_thread = std::thread([this, t0] {
    for (int i = 0; i < m_samples && !m_cancel; i++) {
      if (!m_pt->Pass(&m_cancel)) break;
      std::vector<unsigned char> img;
      m_pt->Image(img);
      {
        std::lock_guard<std::mutex> lk(m_mx);
        m_img.swap(img);
        m_passes = m_pt->Passes();
      }
      const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      QMetaObject::invokeMethod(this, [this, sec] {
        Show();
        m_status->setText(QString("%1 x %2   passate %3 / %4   %5 s").arg(m_w).arg(m_h).arg(m_passes).arg(m_samples).arg(sec, 0, 'f', 1));
      }, Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(this, [this] { m_stop->setEnabled(false); }, Qt::QueuedConnection);
  });
}

RenderWindow::~RenderWindow() {
  m_cancel = true;
  if (m_thread.joinable()) m_thread.join();
}

void RenderWindow::Stop() { m_cancel = true; }

void RenderWindow::closeEvent(QCloseEvent* e) {
  m_cancel = true;
  if (m_thread.joinable()) m_thread.join();
  e->accept();
}

void RenderWindow::resizeEvent(QResizeEvent*) { Show(); }

void RenderWindow::Show() {
  QImage img;
  {
    std::lock_guard<std::mutex> lk(m_mx);
    if (m_img.empty()) return;
    img = QImage(m_img.data(), m_w, m_h, m_w * 3, QImage::Format_RGB888).copy();
  }
  m_view->setPixmap(QPixmap::fromImage(img).scaled(m_view->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

}  // namespace cad::ui
