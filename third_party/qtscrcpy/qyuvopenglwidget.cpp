#include <QCoreApplication>
#include <QOpenGLTexture>
#include <QSurfaceFormat>

#include "qyuvopenglwidget.h"

// 存储顶点坐标和纹理坐标
// 存在一起缓存在vbo
// 使用glVertexAttribPointer指定访问方式即可
static const GLfloat coordinate[] = {
    // 顶点坐标，存储4个xyz坐标
    // 坐标范围为[-1,1],中心点为 0,0
    // 二维图像z始终为0
    // GL_TRIANGLE_STRIP的绘制方式：
    // 使用前3个坐标绘制一个三角形，使用后三个坐标绘制一个三角形，正好为一个矩形
    // x     y     z
    -1.0f,
    -1.0f,
    0.0f,
    1.0f,
    -1.0f,
    0.0f,
    -1.0f,
    1.0f,
    0.0f,
    1.0f,
    1.0f,
    0.0f,

    // 纹理坐标，存储4个xy坐标
    // 坐标范围为[0,1],左下角为 0,0
    0.0f,
    1.0f,
    1.0f,
    1.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f
};

// 顶点着色器
static const QString s_vertShader = R"(
    attribute vec3 vertexIn;    // xyz顶点坐标
    attribute vec2 textureIn;   // xy纹理坐标
    varying vec2 textureOut;    // 传递给片段着色器的纹理坐标
    uniform vec2 sourceOffset;
    uniform vec2 sourceScale;
    void main(void)
    {
        gl_Position = vec4(vertexIn, 1.0);  // 1.0表示vertexIn是一个顶点位置
        textureOut = sourceOffset + textureIn * sourceScale;
    }
)";

// 片段着色器
static QString s_fragShader = R"(
    varying vec2 textureOut;        // 由顶点着色器传递过来的纹理坐标
    uniform sampler2D textureY;     // uniform 纹理单元，利用纹理单元可以使用多个纹理
    uniform sampler2D textureU;     // sampler2D是2D采样器
    uniform sampler2D textureV;     // 声明yuv三个纹理单元
    uniform vec3 Rcoeff;
    uniform vec3 Gcoeff;
    uniform vec3 Bcoeff;
    uniform float yOffset;
    uniform vec2 sourceOffset;
    uniform vec2 sourceScale;
    uniform vec2 yTexel;
    uniform vec2 uvTexel;
    void main(void)
    {
        vec3 yuv;
        vec3 rgb;

        // SDL2 BT709_SHADER_CONSTANTS
        // https://github.com/spurious/SDL-mirror/blob/4ddd4c445aa059bb127e101b74a8c5b59257fbe2/src/render/opengl/SDL_shaders_gl.c#L102

        // 根据指定的纹理textureY和坐标textureOut来采样
        vec2 yCoord = clamp(textureOut, sourceOffset + yTexel * 0.5,
                           sourceOffset + sourceScale - yTexel * 0.5);
        vec2 uvCoord = clamp(textureOut, sourceOffset + uvTexel * 0.5,
                            sourceOffset + sourceScale - uvTexel * 0.5);
        yuv.x = texture2D(textureY, yCoord).r;
        yuv.y = texture2D(textureU, uvCoord).r - 0.5;
        yuv.z = texture2D(textureV, uvCoord).r - 0.5;

        // 采样完转为rgb
        // 减少一些亮度
        yuv.x = yuv.x - yOffset;
        rgb.r = dot(yuv, Rcoeff);
        rgb.g = dot(yuv, Gcoeff);
        rgb.b = dot(yuv, Bcoeff);
        // 输出颜色值
        gl_FragColor = vec4(rgb, 1.0);
    }
)";

QYUVOpenGLWidget::QYUVOpenGLWidget(QWidget *parent) : QOpenGLWidget(parent)
{
    /*
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setColorSpace(QSurfaceFormat::sRGBColorSpace);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setMajorVersion(3);
    format.setMinorVersion(2);
    QSurfaceFormat::setDefaultFormat(format);
    */
}

QYUVOpenGLWidget::~QYUVOpenGLWidget()
{
    makeCurrent();
    m_vbo.destroy();
    deInitTextures();
    doneCurrent();
}

QSize QYUVOpenGLWidget::minimumSizeHint() const
{
    return QSize(50, 50);
}

QSize QYUVOpenGLWidget::sizeHint() const
{
    return size();
}

void QYUVOpenGLWidget::setFrameSize(const QSize &frameSize)
{
    if (m_frameSize != frameSize) {
        m_frameSize = frameSize;
        m_needUpdate = true;
        update();
    }
}

const QSize &QYUVOpenGLWidget::frameSize()
{
    return m_frameSize;
}

void QYUVOpenGLWidget::setSourceRect(const QRect &sourceRect)
{
    m_sourceRect = sourceRect;
    update();
}

void QYUVOpenGLWidget::setColorSpace(bool bt709, bool fullRange)
{
    m_bt709 = bt709;
    m_fullRange = fullRange;
}

void QYUVOpenGLWidget::updateTextures(quint8 *dataY, quint8 *dataU, quint8 *dataV, quint32 linesizeY, quint32 linesizeU, quint32 linesizeV)
{
    // QOpenGLWidget may defer its first paint. Keep the latest complete frame
    // until paintGL has initialized textures and owns the current context.
    const quint8 *data[] = {dataY, dataU, dataV};
    const quint32 stride[] = {linesizeY, linesizeU, linesizeV};
    for (int plane = 0; plane < 3; ++plane) {
        const int width = plane ? (m_frameSize.width()+1)/2 : m_frameSize.width();
        const int height = plane ? (m_frameSize.height()+1)/2 : m_frameSize.height();
        if (!data[plane] || width <= 0 || height <= 0 || stride[plane] < quint32(width)) return;
        m_planes[plane].resize(width * height);
        for (int row = 0; row < height; ++row)
            memcpy(m_planes[plane].data() + row * width, data[plane] + row * stride[plane], size_t(width));
    }
    m_uploadPending = true;
    update();
}

void QYUVOpenGLWidget::initializeGL()
{
    initializeOpenGLFunctions();
    m_textureInited = false;
    m_needUpdate = m_frameSize.isValid();
    m_uploadPending = !m_planes[0].isEmpty();
    glDisable(GL_DEPTH_TEST);

    // 顶点缓冲对象初始化
    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(coordinate, sizeof(coordinate));
    initShader();
    // 设置背景清理色为黑色
    glClearColor(0.0, 0.0, 0.0, 0.0);
    // 清理颜色背景
    glClear(GL_COLOR_BUFFER_BIT);
}

void QYUVOpenGLWidget::paintGL()
{
    m_shaderProgram.bind();

    const QRect full(QPoint(),m_frameSize);
    const QRect source=m_sourceRect.isValid() && full.contains(m_sourceRect) ? m_sourceRect : full;
    const float w=qMax(1,m_frameSize.width()), h=qMax(1,m_frameSize.height());
    m_shaderProgram.setUniformValue("sourceOffset",QVector2D(source.x()/w,source.y()/h));
    m_shaderProgram.setUniformValue("sourceScale",QVector2D(source.width()/w,source.height()/h));
    m_shaderProgram.setUniformValue("yTexel",QVector2D(1.0f/w,1.0f/h));
    m_shaderProgram.setUniformValue("uvTexel",QVector2D(1.0f/qMax(1,(m_frameSize.width()+1)/2),1.0f/qMax(1,(m_frameSize.height()+1)/2)));

    const float y = m_fullRange ? 1.0f : 255.0f / 219.0f;
    const float c = m_fullRange ? 1.0f : 255.0f / 224.0f;
    m_shaderProgram.setUniformValue("yOffset", m_fullRange ? 0.0f : 16.0f / 255.0f);
    m_shaderProgram.setUniformValue("Rcoeff", QVector3D(y, 0, c * (m_bt709 ? 1.5748f : 1.402f)));
    m_shaderProgram.setUniformValue("Gcoeff", QVector3D(y, c * (m_bt709 ? -0.187324f : -0.344136f), c * (m_bt709 ? -0.468124f : -0.714136f)));
    m_shaderProgram.setUniformValue("Bcoeff", QVector3D(y, c * (m_bt709 ? 1.8556f : 1.772f), 0));

    if (m_needUpdate) {
        deInitTextures();
        initTextures();
        m_needUpdate = false;
    }

    if (m_textureInited) {
        if (m_uploadPending) {
            for (int plane = 0; plane < 3; ++plane)
                updateTexture(m_texture[plane], quint32(plane), reinterpret_cast<quint8 *>(m_planes[plane].data()),
                              quint32(plane ? (m_frameSize.width()+1)/2 : m_frameSize.width()));
            m_uploadPending = false;
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texture[0]);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_texture[1]);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_texture[2]);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    m_shaderProgram.release();
}

void QYUVOpenGLWidget::resizeGL(int width, int height)
{
    glViewport(0, 0, width, height);
    repaint();
}

void QYUVOpenGLWidget::initShader()
{
    // opengles的float、int等要手动指定精度
    if (QCoreApplication::testAttribute(Qt::AA_UseOpenGLES)) {
        s_fragShader.prepend(R"(
                             precision mediump int;
                             precision mediump float;
                             )");
    }
    m_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, s_vertShader);
    m_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, s_fragShader);
    m_shaderProgram.link();
    m_shaderProgram.bind();

    // 指定顶点坐标在vbo中的访问方式
    // 参数解释：顶点坐标在shader中的参数名称，顶点坐标为float，起始偏移为0，顶点坐标类型为vec3，步幅为3个float
    m_shaderProgram.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 3, 3 * sizeof(float));
    // 启用顶点属性
    m_shaderProgram.enableAttributeArray("vertexIn");

    // 指定纹理坐标在vbo中的访问方式
    // 参数解释：纹理坐标在shader中的参数名称，纹理坐标为float，起始偏移为12个float（跳过前面存储的12个顶点坐标），纹理坐标类型为vec2，步幅为2个float
    m_shaderProgram.setAttributeBuffer("textureIn", GL_FLOAT, 12 * sizeof(float), 2, 2 * sizeof(float));
    m_shaderProgram.enableAttributeArray("textureIn");

    // 关联片段着色器中的纹理单元和opengl中的纹理单元（opengl一般提供16个纹理单元）
    m_shaderProgram.setUniformValue("textureY", 0);
    m_shaderProgram.setUniformValue("textureU", 1);
    m_shaderProgram.setUniformValue("textureV", 2);
}

void QYUVOpenGLWidget::initTextures()
{
    // 创建纹理
    glGenTextures(1, &m_texture[0]);
    glBindTexture(GL_TEXTURE_2D, m_texture[0]);
    // 设置纹理缩放时的策略
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // 设置st方向上纹理超出坐标时的显示策略
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, m_frameSize.width(), m_frameSize.height(), 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);

    glGenTextures(1, &m_texture[1]);
    glBindTexture(GL_TEXTURE_2D, m_texture[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, (m_frameSize.width() + 1) / 2, (m_frameSize.height() + 1) / 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);

    glGenTextures(1, &m_texture[2]);
    glBindTexture(GL_TEXTURE_2D, m_texture[2]);
    // 设置纹理缩放时的策略
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // 设置st方向上纹理超出坐标时的显示策略
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, (m_frameSize.width() + 1) / 2, (m_frameSize.height() + 1) / 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);

    m_textureInited = true;
}

void QYUVOpenGLWidget::deInitTextures()
{
    if (QOpenGLFunctions::isInitialized(QOpenGLFunctions::d_ptr)) {
        glDeleteTextures(3, m_texture);
    }

    memset(m_texture, 0, sizeof(m_texture));
    m_textureInited = false;
}

void QYUVOpenGLWidget::updateTexture(GLuint texture, quint32 textureType, quint8 *pixels, quint32 stride)
{
    if (!pixels)
        return;

    QSize size = 0 == textureType ? m_frameSize : QSize((m_frameSize.width()+1)/2, (m_frameSize.height()+1)/2);

    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(stride));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size.width(), size.height(), GL_LUMINANCE, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}
