//
//  blur_split_screen.h
//  opengl
//
//  Created by wlanjie on 2019/8/31.
//  Copyright © 2019 com.wlanjie.opengl. All rights reserved.
//

#ifndef blur_split_screen_h
#define blur_split_screen_h

#include "frame_buffer.h"
#include "gaussian_blur.h"

namespace trinity {

static const char* SAMPLE_VERTEX_SHADER =
    "attribute vec4 position;\n"
    "attribute vec2 inputTextureCoordinate;\n"
    "varying vec2 textureCoordinate;\n"
    "void main() {\n"
    "   gl_Position = position;\n"
    "   textureCoordinate = (inputTextureCoordinate.xy - 0.5) / 1.5 + 0.5;\n"
    "}\n";
    
static const char* BLEND_FRAGMENT_SHADER =
    "#ifdef GL_ES\n"
    "precision highp float;\n"
    "#endif\n"
    "varying vec2 textureCoordinate;\n"
    "uniform sampler2D inputImageTexture;\n"
    "uniform sampler2D inputImageTextureBlurred;\n"
    "void main() {\n"
    "   int col = int(textureCoordinate.y * 3.0);\n"
    "   vec2 textureCoordinateToUse = textureCoordinate;\n"
    "   textureCoordinateToUse.y = (textureCoordinate.y - float(col) / 3.0) * 3.0;\n"
    "   textureCoordinateToUse.y = textureCoordinateToUse.y / 3.0 + 1.0 / 3.0;\n"
    "   if (col == 1) {\n"
    "       gl_FragColor = texture2D(inputImageTexture, textureCoordinateToUse);\n"
    "   } else {\n"
    "       gl_FragColor = texture2D(inputImageTextureBlurred, textureCoordinate);\n"
    "   }\n"
    "}\n";
  
class BlurSplitScreen : public FrameBuffer {
 public:
    BlurSplitScreen(int width, int height) : FrameBuffer(width, height, SAMPLE_VERTEX_SHADER, BLEND_FRAGMENT_SHADER) {
        blur_texture_id_ = 0;
        gaussian_blur_ = new GaussianBlur(width, height, 1.0f);
    }
    
    ~BlurSplitScreen() {
        delete gaussian_blur_;
        gaussian_blur_ = nullptr;
    }
    
    int OnDrawFrame(int texture_id, uint64_t current_time = 0) {
        blur_texture_id_ = gaussian_blur_->OnDrawFrame(texture_id);
        return FrameBuffer::OnDrawFrame(texture_id, current_time);
    }
    
 protected:
    virtual void RunOnDrawTasks() {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, blur_texture_id_);
        SetInt("inputImageTextureBlurred", 1);
    }
 private:
    GaussianBlur* gaussian_blur_;
    int blur_texture_id_;
};
  
}

#endif /* blur_split_screen_h */