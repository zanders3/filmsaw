#pragma once

int pfd_open_dialog(const char* title, const char** filters, int numfilters, char* filepath_out, int filepath_out_len);
int pfd_save_dialog(const char* title, const char* defaultpath, const char** filters, int numfilters,
                    char* filepath_out, int filepath_out_len);
