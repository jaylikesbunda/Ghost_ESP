from PIL import Image

def generate_lvgl_code(image_path, canvas_name='canvas', lvgl_color_func='lv_color_hex'):
    # Extract the image name without the file extension for function naming
    image_name = image_path.split('/')[-1].split('.')[0]

    # Open the image and convert to RGB format
    img = Image.open(image_path).convert('RGB')
    width, height = img.size
    pixels = img.load()

    # Initialize the LVGL code
    lvgl_code = []
    lvgl_code.append(f'// Auto-generated LVGL code for rendering the sprite "{image_path}"')
    lvgl_code.append(f'// Canvas: {canvas_name}, Dimensions: {width}x{height}')
    lvgl_code.append(f'void create_{image_name}(lv_obj_t *parent, int angle) {{')
    lvgl_code.append(f'    lv_obj_t *{canvas_name} = lv_canvas_create(parent);')
    lvgl_code.append(f'    lv_obj_set_size({canvas_name}, {width}, {height});')
    lvgl_code.append(f'    static lv_color_t {canvas_name}_buffer[LV_CANVAS_BUF_SIZE_TRUE_COLOR({width}, {height})];')
    lvgl_code.append(f'    lv_canvas_set_buffer({canvas_name}, {canvas_name}_buffer, {width}, {height}, LV_IMG_CF_TRUE_COLOR);')
    lvgl_code.append(f'    lv_img_set_angle({canvas_name}, angle * 10);  // Angle is in tenths of degrees')

    # Generate code to draw each pixel
    for y in range(height):
        for x in range(width):
            r, g, b = pixels[x, y]
            hex_color = f'0x{r:02X}{g:02X}{b:02X}'
            lvgl_code.append(f'    lv_canvas_set_px({canvas_name}, {x}, {y}, {lvgl_color_func}({hex_color}));')

    lvgl_code.append('}')
    return '\n'.join(lvgl_code)

# Example usage:
image_path = input("Path to your image: ")
lvgl_code = generate_lvgl_code(image_path)

# Write the LVGL code to a file (optional)
with open('lvgl_sprite_code.c', 'w') as f:
    f.write(lvgl_code)

print("LVGL code generated and saved to 'lvgl_sprite_code.c'.")
