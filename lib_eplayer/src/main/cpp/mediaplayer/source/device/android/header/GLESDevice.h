
#ifndef EPLAYER_GLESDEVICE_H
#define EPLAYER_GLESDEVICE_H
#include "VideoDevice.h"
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <Filter.h>
#include <RenderNodeList.h>
#include "render/common/header/EglHelper.h"
#include "InputRenderNode.h"

class GLESDevice : public VideoDevice {
public:
    GLESDevice();

    virtual ~GLESDevice();

    void surfaceCreated(ANativeWindow *window);

    //Surface的大小发生改变
    void surfaceChanged(int width, int height);

    void setTimeStamp(double timeStamp) override;

    void terminate() override;

    void terminate(bool releaseContext);

    void onInitTexture(int width, int height, TextureFormat format, BlendMode blendMode,
                       int rotate) override;

    int onUpdateYUV(uint8_t *yData, int yPitch, uint8_t *uData, int uPitch,
                    uint8_t *vData, int vPitch) override;

    int onUpdateARGB(uint8_t *rgba, int pitch) override;

    int onRequestRender(bool flip) override;


    // 改变滤镜
    void changeFilter(RenderNodeType type, const char *filterName);

    // 改变滤镜
    void changeFilter(RenderNodeType type, const int id);

    // 设置水印
    void setWatermark(uint8_t *watermarkPixel, size_t length, GLint width, GLint height, GLfloat scale,
                      GLint location);

private:
    void resetVertices();

    void resetTexVertices();

private:
    Mutex mMutex;
    Condition mCondition;

    ANativeWindow *mWindow;             // Surface窗口
    int mSurfaceWidth;                  // 窗口宽度
    int mSurfaceHeight;                 // 窗口高度
    EGLSurface eglSurface;              // eglSurface
    EglHelper *eglHelper;               // EGL帮助器
    bool mSurfaceReset;                 // 重新设置Surface
    bool mHasSurface;                   // 是否存在Surface
    bool mHaveEGLSurface;               // EGLSurface
    bool mHaveEGlContext;               // 释放资源

    Texture *mVideoTexture;             // 视频纹理
    InputRenderNode *mRenderNode;       // 输入渲染结点
    float vertices[8];                  // 顶点坐标
    float textureVertices[8];           // 纹理坐标

    RenderNodeList *nodeList;           // 滤镜链
    FilterInfo filterInfo;              // 滤镜信息
    bool filterChange;                  // 切换滤镜


};
#endif //EPLAYER_GLESDEVICE_H
