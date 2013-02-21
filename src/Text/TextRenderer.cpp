/*
    Copyright © 2010, 2011, 2012 Vladimír Vondruš <mosra@centrum.cz>

    This file is part of Magnum.

    Magnum is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License version 3
    only, as published by the Free Software Foundation.

    Magnum is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License version 3 for more details.
*/

#include "TextRenderer.h"

#include <hb.h>

#include "Math/Point2D.h"
#include "Math/Point3D.h"
#include "Context.h"
#include "Extensions.h"
#include "Mesh.h"
#include "Swizzle.h"
#include "Shaders/AbstractTextShader.h"
#include "Text/Font.h"

namespace Magnum { namespace Text {

namespace {

class TextLayouter {
    public:
        TextLayouter(Font& font, const GLfloat size, const std::string& text);

        ~TextLayouter();

        inline std::uint32_t glyphCount() { return _glyphCount; }

        std::tuple<Rectangle, Rectangle, Vector2> renderGlyph(const Vector2& cursorPosition, const std::uint32_t i);

    private:
        const Font& font;
        const GLfloat size;
        hb_buffer_t* buffer;
        hb_glyph_info_t* glyphInfo;
        hb_glyph_position_t* glyphPositions;
        std::uint32_t _glyphCount;
};

TextLayouter::TextLayouter(Font& font, const GLfloat size, const std::string& text): font(font), size(size) {
    /* Prepare HarfBuzz buffer */
    buffer = hb_buffer_create();
    hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
    hb_buffer_set_script(buffer, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buffer, hb_language_from_string("en", 2));

    /* Layout the text */
    hb_buffer_add_utf8(buffer, text.c_str(), -1, 0, -1);
    hb_shape(font.font(), buffer, nullptr, 0);

    glyphInfo = hb_buffer_get_glyph_infos(buffer, &_glyphCount);
    glyphPositions = hb_buffer_get_glyph_positions(buffer, &_glyphCount);
}

TextLayouter::~TextLayouter() {
    /* Destroy HarfBuzz buffer */
    hb_buffer_destroy(buffer);
}

std::tuple<Rectangle, Rectangle, Vector2> TextLayouter::renderGlyph(const Vector2& cursorPosition, const std::uint32_t i) {
    /* Position of the texture in the resulting glyph, texture coordinates */
    Rectangle texturePosition, textureCoordinates;
    std::tie(texturePosition, textureCoordinates) = font[glyphInfo[i].codepoint];

    /* Glyph offset and advance to next glyph in normalized coordinates */
    Vector2 offset = Vector2(glyphPositions[i].x_offset,
                             glyphPositions[i].y_offset)/(64*font.size());
    Vector2 advance = Vector2(glyphPositions[i].x_advance,
                              glyphPositions[i].y_advance)/(64*font.size());

    /* Absolute quad position, composed from cursor position, glyph offset
        and texture position, denormalized to requested text size */
    Rectangle quadPosition = Rectangle::fromSize(
        (cursorPosition + offset + Vector2(texturePosition.left(), texturePosition.bottom()))*size,
        texturePosition.size()*size);

    return std::make_tuple(quadPosition, textureCoordinates, advance);
}

template<class T> void createIndices(void* output, const std::uint32_t glyphCount) {
    T* const out = reinterpret_cast<T*>(output);
    for(std::uint32_t i = 0; i != glyphCount; ++i) {
        /* 0---2 2
           |  / /|
           | / / |
           |/ /  |
           1 1---3 */

        const T vertex = i*4;
        const T pos = i*6;
        out[pos]   = vertex;
        out[pos+1] = vertex+1;
        out[pos+2] = vertex+2;
        out[pos+3] = vertex+1;
        out[pos+4] = vertex+3;
        out[pos+5] = vertex+2;
    }
}

template<std::uint8_t dimensions> typename DimensionTraits<dimensions>::PointType point(const Vector2& vec);

template<> inline Point2D point<2>(const Vector2& vec) {
    return swizzle<'x', 'y', '1'>(vec);
}

template<> inline Point3D point<3>(const Vector2& vec) {
    return swizzle<'x', 'y', '0', '1'>(vec);
}

template<std::uint8_t dimensions> struct Vertex {
    typename DimensionTraits<dimensions>::PointType position;
    Vector2 texcoords;
};

}

template<std::uint8_t dimensions> std::tuple<std::vector<typename DimensionTraits<dimensions>::PointType>, std::vector<Vector2>, std::vector<std::uint32_t>, Rectangle> TextRenderer<dimensions>::render(Font& font, GLfloat size, const std::string& text) {
    TextLayouter layouter(font, size, text);

    const std::uint32_t vertexCount = layouter.glyphCount()*4;

    /* Output data */
    std::vector<typename DimensionTraits<dimensions>::PointType> positions;
    std::vector<Vector2> texcoords;
    positions.reserve(vertexCount);
    texcoords.reserve(vertexCount);

    /* Render all glyphs */
    Vector2 cursorPosition;
    for(std::uint32_t i = 0; i != layouter.glyphCount(); ++i) {
        /* Position of the texture in the resulting glyph, texture coordinates */
        Rectangle quadPosition, textureCoordinates;
        Vector2 advance;
        std::tie(quadPosition, textureCoordinates, advance) = layouter.renderGlyph(cursorPosition, i);

        positions.insert(positions.end(), {
            point<dimensions>(quadPosition.topLeft()),
            point<dimensions>(quadPosition.bottomLeft()),
            point<dimensions>(quadPosition.topRight()),
            point<dimensions>(quadPosition.bottomRight()),
        });
        texcoords.insert(texcoords.end(), {
            textureCoordinates.topLeft(),
            textureCoordinates.bottomLeft(),
            textureCoordinates.topRight(),
            textureCoordinates.bottomRight()
        });

        /* Advance cursor position to next character */
        cursorPosition += advance;
    }

    /* Create indices */
    std::vector<std::uint32_t> indices(layouter.glyphCount()*6);
    createIndices<std::uint32_t>(indices.data(), layouter.glyphCount());

    /* Rendered rectangle */
    Rectangle rectangle;
    if(layouter.glyphCount()) rectangle = {positions[1].xy(), positions[positions.size()-2].xy()};

    return std::make_tuple(std::move(positions), std::move(texcoords), std::move(indices), rectangle);
}

template<std::uint8_t dimensions> std::tuple<Mesh, Rectangle> TextRenderer<dimensions>::render(Font& font, GLfloat size, const std::string& text, Buffer* vertexBuffer, Buffer* indexBuffer, Buffer::Usage usage) {
    TextLayouter layouter(font, size, text);

    const std::uint32_t vertexCount = layouter.glyphCount()*4;
    const std::uint32_t indexCount = layouter.glyphCount()*6;

    /* Vertex buffer */
    std::vector<Vertex<dimensions>> vertices;
    vertices.reserve(vertexCount);

    /* Render all glyphs */
    Vector2 cursorPosition;
    for(std::uint32_t i = 0; i != layouter.glyphCount(); ++i) {
        /* Position of the texture in the resulting glyph, texture coordinates */
        Rectangle quadPosition, textureCoordinates;
        Vector2 advance;
        std::tie(quadPosition, textureCoordinates, advance) = layouter.renderGlyph(cursorPosition, i);

        vertices.insert(vertices.end(), {
            {point<dimensions>(quadPosition.topLeft()), textureCoordinates.topLeft()},
            {point<dimensions>(quadPosition.bottomLeft()), textureCoordinates.bottomLeft()},
            {point<dimensions>(quadPosition.topRight()), textureCoordinates.topRight()},
            {point<dimensions>(quadPosition.bottomRight()), textureCoordinates.bottomRight()}
        });

        /* Advance cursor position to next character */
        cursorPosition += advance;
    }
    vertexBuffer->setData(vertices, usage);

    /* Fill index buffer */
    Mesh::IndexType indexType;
    std::size_t indicesSize;
    char* indices;
    if(vertexCount < 255) {
        indexType = Mesh::IndexType::UnsignedByte;
        indicesSize = indexCount*sizeof(GLushort);
        indices = new char[indicesSize];
        createIndices<GLubyte>(indices, layouter.glyphCount());
    } else if(vertexCount < 65535) {
        indexType = Mesh::IndexType::UnsignedShort;
        indicesSize = indexCount*sizeof(GLushort);
        indices = new char[indicesSize];
        createIndices<GLushort>(indices, layouter.glyphCount());
    } else {
        indexType = Mesh::IndexType::UnsignedInt;
        indicesSize = indexCount*sizeof(GLuint);
        indices = new char[indicesSize];
        createIndices<GLuint>(indices, layouter.glyphCount());
    }
    indexBuffer->setData(indicesSize, indices, usage);
    delete indices;

    /* Rendered rectangle */
    Rectangle rectangle;
    if(layouter.glyphCount()) rectangle = {vertices[1].position.xy(), vertices[vertices.size()-2].position.xy()};

    /* Configure mesh */
    Mesh mesh;
    mesh.setPrimitive(Mesh::Primitive::Triangles)
        ->setIndexCount(indexCount)
        ->addInterleavedVertexBuffer(vertexBuffer, 0,
            typename Shaders::AbstractTextShader<dimensions>::Position(),
            typename Shaders::AbstractTextShader<dimensions>::TextureCoordinates())
        ->setIndexBuffer(indexBuffer, 0, indexType, 0, vertexCount);

    return std::make_tuple(std::move(mesh), rectangle);
}

template<std::uint8_t dimensions> TextRenderer<dimensions>::TextRenderer(Font& font, const GLfloat size): font(font), size(size), _capacity(0), vertexBuffer(Buffer::Target::Array), indexBuffer(Buffer::Target::ElementArray) {
    #ifndef MAGNUM_TARGET_GLES
    MAGNUM_ASSERT_EXTENSION_SUPPORTED(Extensions::GL::ARB::map_buffer_range);
    #else
    #ifdef MAGNUM_TARGET_GLES2
    MAGNUM_ASSERT_EXTENSION_SUPPORTED(Extensions::GL::EXT::map_buffer_range);
    #endif
    #endif

    _mesh.setPrimitive(Mesh::Primitive::Triangles)
        ->addInterleavedVertexBuffer(&vertexBuffer, 0,
            typename Shaders::AbstractTextShader<dimensions>::Position(),
            typename Shaders::AbstractTextShader<dimensions>::TextureCoordinates());
}

template<std::uint8_t dimensions> void TextRenderer<dimensions>::reserve(const uint32_t glyphCount, const Buffer::Usage vertexBufferUsage, const Buffer::Usage indexBufferUsage) {
    _capacity = glyphCount;

    const std::uint32_t vertexCount = glyphCount*4;
    const std::uint32_t indexCount = glyphCount*6;

    /* Allocate vertex buffer, reset vertex count */
    vertexBuffer.setData(vertexCount*sizeof(Vertex<dimensions>), nullptr, vertexBufferUsage);
    _mesh.setVertexCount(0);

    /* Allocate index buffer, reset index count and reconfigure buffer binding */
    Mesh::IndexType indexType;
    std::size_t indicesSize;
    if(vertexCount < 255) {
        indexType = Mesh::IndexType::UnsignedByte;
        indicesSize = indexCount*sizeof(GLushort);
    } else if(vertexCount < 65535) {
        indexType = Mesh::IndexType::UnsignedShort;
        indicesSize = indexCount*sizeof(GLushort);
    } else {
        indexType = Mesh::IndexType::UnsignedInt;
        indicesSize = indexCount*sizeof(GLuint);
    }
    indexBuffer.setData(indicesSize, nullptr, indexBufferUsage);
    _mesh.setIndexCount(0)
        ->setIndexBuffer(&indexBuffer, 0, indexType, 0, vertexCount);

    /* Prefill index buffer */
    void* indices = indexBuffer.map(0, indicesSize, Buffer::MapFlag::InvalidateBuffer|Buffer::MapFlag::Write);
    if(vertexCount < 255)
        createIndices<GLubyte>(indices, glyphCount);
    else if(vertexCount < 65535)
        createIndices<GLushort>(indices, glyphCount);
    else
        createIndices<GLuint>(indices, glyphCount);
    CORRADE_INTERNAL_ASSERT_OUTPUT(indexBuffer.unmap());
}

template<std::uint8_t dimensions> void TextRenderer<dimensions>::render(const std::string& text) {
    TextLayouter layouter(font, size, text);

    CORRADE_ASSERT(layouter.glyphCount() <= _capacity, "Text::TextRenderer::render(): capacity" << _capacity << "too small to render" << layouter.glyphCount() << "glyphs", );

    /* Render all glyphs */
    Vertex<dimensions>* const vertices = static_cast<Vertex<dimensions>*>(vertexBuffer.map(0, layouter.glyphCount()*4*sizeof(Vertex<dimensions>),
        Buffer::MapFlag::InvalidateBuffer|Buffer::MapFlag::Write));
    Vector2 cursorPosition;
    for(std::uint32_t i = 0; i != layouter.glyphCount(); ++i) {
        /* Position of the texture in the resulting glyph, texture coordinates */
        Rectangle quadPosition, textureCoordinates;
        Vector2 advance;
        std::tie(quadPosition, textureCoordinates, advance) = layouter.renderGlyph(cursorPosition, i);

        if(i == 0)
            _rectangle.bottomLeft() = quadPosition.bottomLeft();
        else if(i == layouter.glyphCount()-1)
            _rectangle.topRight() = quadPosition.topRight();

        const std::size_t vertex = i*4;
        vertices[vertex]   = {point<dimensions>(quadPosition.topLeft()), textureCoordinates.topLeft()};
        vertices[vertex+1] = {point<dimensions>(quadPosition.bottomLeft()), textureCoordinates.bottomLeft()};
        vertices[vertex+2] = {point<dimensions>(quadPosition.topRight()), textureCoordinates.topRight()};
        vertices[vertex+3] = {point<dimensions>(quadPosition.bottomRight()), textureCoordinates.bottomRight()};

        /* Advance cursor position to next character */
        cursorPosition += advance;
    }
    CORRADE_INTERNAL_ASSERT_OUTPUT(vertexBuffer.unmap());

    /* Update index count */
    _mesh.setIndexCount(layouter.glyphCount()*6);
}

template class TextRenderer<2>;
template class TextRenderer<3>;

}}
