#include "project.h"
#include "config.h"
#include "terminal.h"
#include "filesystem.h"
#include "directories.h"
#include <fstream>
#include "menu.h"
#include "notebook.h"
#include "selection_dialog.h"
#ifdef JUCI_ENABLE_DEBUG
#include "debug_lldb.h"
#endif
#include "info.h"

boost::filesystem::path Project::debug_last_stop_file_path;
std::unordered_map<std::string, std::string> Project::run_arguments;
std::unordered_map<std::string, std::string> Project::debug_run_arguments;
std::atomic<bool> Project::compiling(false);
std::atomic<bool> Project::debugging(false);
std::pair<boost::filesystem::path, std::pair<int, int> > Project::debug_stop;
std::string Project::debug_status;
std::unique_ptr<Project::Base> Project::current;
#ifdef JUCI_ENABLE_DEBUG
std::unordered_map<std::string, Project::Clang::DebugOptions> Project::Clang::debug_options;
#endif

Gtk::Label &Project::debug_status_label() {
  static Gtk::Label label;
  return label;
}

void Project::save_files(const boost::filesystem::path &path) {
  for(size_t c=0;c<Notebook::get().size();c++) {
    auto view=Notebook::get().get_view(c);
    if(view->get_buffer()->get_modified()) {
      if(filesystem::file_in_path(view->file_path, path))
        Notebook::get().save(c);
    }
  }
}

void Project::on_save(size_t index) {
  auto view=Notebook::get().get_view(index);
  if(!view)
    return;
  boost::filesystem::path build_path;
  if(view->language && view->language->get_id()=="cmake") {
    if(view->file_path.filename()=="CMakeLists.txt")
      build_path=view->file_path;
    else
      build_path=filesystem::find_file_in_path_parents("CMakeLists.txt", view->file_path.parent_path());
  }
  else if(view->language && view->language->get_id()=="meson") {
    if(view->file_path.filename()=="meson.build")
      build_path=view->file_path;
    else
      build_path=filesystem::find_file_in_path_parents("meson.build", view->file_path.parent_path());
  }
  
  if(!build_path.empty()) {
    auto build=Build::create(build_path);
    if(dynamic_cast<CMakeBuild*>(build.get()) || dynamic_cast<MesonBuild*>(build.get())) {
      build->update_default(true);
      if(boost::filesystem::exists(build->get_debug_path()))
        build->update_debug(true);
      
      for(size_t c=0;c<Notebook::get().size();c++) {
        auto source_view=Notebook::get().get_view(c);
        if(auto source_clang_view=dynamic_cast<Source::ClangView*>(source_view)) {
          if(filesystem::file_in_path(source_clang_view->file_path, build->project_path))
            source_clang_view->full_reparse_needed=true;
        }
      }
    }
  }
}

void Project::debug_update_status(const std::string &new_debug_status) {
  debug_status=new_debug_status;
  if(debug_status.empty())
    debug_status_label().set_text("");
  else
    debug_status_label().set_text(debug_status);
  debug_activate_menu_items();
}

void Project::debug_activate_menu_items() {
  auto &menu=Menu::get();
  auto view=Notebook::get().get_current_view();
  menu.actions["debug_stop"]->set_enabled(!debug_status.empty());
  menu.actions["debug_kill"]->set_enabled(!debug_status.empty());
  menu.actions["debug_step_over"]->set_enabled(!debug_status.empty());
  menu.actions["debug_step_into"]->set_enabled(!debug_status.empty());
  menu.actions["debug_step_out"]->set_enabled(!debug_status.empty());
  menu.actions["debug_backtrace"]->set_enabled(!debug_status.empty());
  menu.actions["debug_show_variables"]->set_enabled(!debug_status.empty());
  menu.actions["debug_run_command"]->set_enabled(!debug_status.empty());
  menu.actions["debug_toggle_breakpoint"]->set_enabled(view && view->toggle_breakpoint);
  menu.actions["debug_goto_stop"]->set_enabled(!debug_status.empty());
}

void Project::debug_update_stop() {
  if(!debug_last_stop_file_path.empty()) {
    for(size_t c=0;c<Notebook::get().size();c++) {
      auto view=Notebook::get().get_view(c);
      if(view->file_path==debug_last_stop_file_path) {
        view->get_source_buffer()->remove_source_marks(view->get_buffer()->begin(), view->get_buffer()->end(), "debug_stop");
        view->get_source_buffer()->remove_source_marks(view->get_buffer()->begin(), view->get_buffer()->end(), "debug_breakpoint_and_stop");
        break;
      }
    }
  }
  //Add debug stop source mark
  debug_last_stop_file_path.clear();
  for(size_t c=0;c<Notebook::get().size();c++) {
    auto view=Notebook::get().get_view(c);
    if(view->file_path==debug_stop.first) {
      if(debug_stop.second.first<view->get_buffer()->get_line_count()) {
        auto iter=view->get_buffer()->get_iter_at_line(debug_stop.second.first);
        view->get_source_buffer()->create_source_mark("debug_stop", iter);
        if(view->get_source_buffer()->get_source_marks_at_iter(iter, "debug_breakpoint").size()>0)
          view->get_source_buffer()->create_source_mark("debug_breakpoint_and_stop", iter);
        debug_last_stop_file_path=debug_stop.first;
      }
      break;
    }
  }
}

std::unique_ptr<Project::Base> Project::create() {
  std::unique_ptr<Project::Build> build;
  
  if(auto view=Notebook::get().get_current_view()) {
    build=Build::create(view->file_path);
    if(view->language) {
      auto language_id=view->language->get_id();
      if(language_id=="markdown")
        return std::unique_ptr<Project::Base>(new Project::Markdown(std::move(build)));
      if(language_id=="python")
        return std::unique_ptr<Project::Base>(new Project::Python(std::move(build)));
      if(language_id=="js")
        return std::unique_ptr<Project::Base>(new Project::JavaScript(std::move(build)));
      if(language_id=="html")
        return std::unique_ptr<Project::Base>(new Project::HTML(std::move(build)));
    }
  }
  else
    build=Build::create(Directories::get().path);
  
  if(dynamic_cast<CMakeBuild*>(build.get()) || dynamic_cast<MesonBuild*>(build.get()))
    return std::unique_ptr<Project::Base>(new Project::Clang(std::move(build)));
  else
    return std::unique_ptr<Project::Base>(new Project::Base(std::move(build)));
}

std::pair<std::string, std::string> Project::Base::get_run_arguments() {
  Info::get().print("Could not find a supported project");
  return {"", ""};
}

void Project::Base::compile() {
  Info::get().print("Could not find a supported project");
}

void Project::Base::compile_and_run() {
  Info::get().print("Could not find a supported project");
}

void Project::Base::recreate_build() {
  Info::get().print("Could not find a supported project");
}

std::pair<std::string, std::string> Project::Base::debug_get_run_arguments() {
  Info::get().print("Could not find a supported project");
  return {"", ""};
}

void Project::Base::debug_start() {
  Info::get().print("Could not find a supported project");
}

#ifdef JUCI_ENABLE_DEBUG
Project::Clang::DebugOptions::DebugOptions() : Base::DebugOptions() {
  remote_enabled.set_active(false);
  remote_enabled.set_label("Enabled");
  remote_enabled.signal_clicked().connect([this] {
    remote_host.set_sensitive(remote_enabled.get_active());
  });
  remote_host.set_sensitive(false);
  remote_host.set_placeholder_text("host:port");
  remote_host.signal_activate().connect([this] {
    set_visible(false);
  });
  
  auto remote_vbox=Gtk::manage(new Gtk::Box(Gtk::Orientation::ORIENTATION_VERTICAL));
  remote_vbox->pack_start(remote_enabled, true, true);
  remote_vbox->pack_end(remote_host, true, true);
  
  auto remote_frame=Gtk::manage(new Gtk::Frame());
  remote_frame->set_label("Remote Debugging");
  remote_frame->add(*remote_vbox);
  
  vbox.pack_end(*remote_frame, true, true);
  
  show_all();
  set_visible(false);
}
#endif

std::pair<std::string, std::string> Project::Clang::get_run_arguments() {
  auto build_path=build->get_default_path();
  if(build_path.empty())
    return {"", ""};
  
  auto project_path=build->project_path.string();
  auto run_arguments_it=run_arguments.find(project_path);
  std::string arguments;
  if(run_arguments_it!=run_arguments.end())
    arguments=run_arguments_it->second;
  
  if(arguments.empty()) {
    auto view=Notebook::get().get_current_view();
    auto executable=build->get_executable(view?view->file_path:Directories::get().path);
    
    if(!executable.empty())
      arguments=filesystem::escape_argument(filesystem::get_short_path(executable).string());
    else
      arguments=filesystem::escape_argument(filesystem::get_short_path(build->get_default_path()).string());
  }
  
  return {project_path, arguments};
}

void Project::Clang::compile() {
  auto default_build_path=build->get_default_path();
  if(default_build_path.empty() || !build->update_default())
    return;
  
  if(Config::get().project.clear_terminal_on_compile)
    Terminal::get().clear();
  
  compiling=true;
  Terminal::get().print("Compiling project "+filesystem::get_short_path(build->project_path).string()+"\n");
  std::string compile_command;
  if(dynamic_cast<CMakeBuild*>(build.get()))
    compile_command=Config::get().project.cmake.compile_command;
  else if(dynamic_cast<MesonBuild*>(build.get()))
    compile_command=Config::get().project.meson.compile_command;
  Terminal::get().async_process(compile_command, default_build_path, [this](int exit_status) {
    compiling=false;
  });
}

void Project::Clang::compile_and_run() {
  auto default_build_path=build->get_default_path();
  if(default_build_path.empty() || !build->update_default())
    return;
  
  auto project_path=build->project_path;
  
  auto run_arguments_it=run_arguments.find(project_path.string());
  std::string arguments;
  if(run_arguments_it!=run_arguments.end())
    arguments=run_arguments_it->second;
  
  if(arguments.empty()) {
    auto view=Notebook::get().get_current_view();
    auto executable=build->get_executable(view?view->file_path:Directories::get().path);
    if(executable.empty()) {
      Terminal::get().print("Warning: could not find executable.\n");
      Terminal::get().print("Solution: either use Project Set Run Arguments, or open a source file within a directory where an executable is defined.\n", true);
      return;
    }
    arguments=filesystem::escape_argument(filesystem::get_short_path(executable).string());
  }
  
  if(Config::get().project.clear_terminal_on_compile)
    Terminal::get().clear();
  
  compiling=true;
  Terminal::get().print("Compiling and running "+arguments+"\n");
  std::string compile_command;
  if(dynamic_cast<CMakeBuild*>(build.get()))
    compile_command=Config::get().project.cmake.compile_command;
  else if(dynamic_cast<MesonBuild*>(build.get()))
    compile_command=Config::get().project.meson.compile_command;
  Terminal::get().async_process(compile_command, default_build_path, [this, arguments, project_path](int exit_status){
    compiling=false;
    if(exit_status==EXIT_SUCCESS) {
      Terminal::get().async_process(arguments, project_path, [this, arguments](int exit_status){
        Terminal::get().async_print(arguments+" returned: "+std::to_string(exit_status)+'\n');
      });
    }
  });
}

void Project::Clang::recreate_build() {
  auto default_build_path=build->get_default_path();
  if(default_build_path.empty())
    return;
  
  auto debug_build_path=build->get_debug_path();
  bool has_default_build=boost::filesystem::exists(default_build_path);
  bool has_debug_build=!debug_build_path.empty() && boost::filesystem::exists(debug_build_path);
  
  if(has_default_build || has_debug_build) {
    Gtk::MessageDialog dialog(*static_cast<Gtk::Window*>(Notebook::get().get_toplevel()), "Recreate Build", false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_YES_NO);
    dialog.set_default_response(Gtk::RESPONSE_NO);
    std::string message="Are you sure you want to recreate ";
    if(has_default_build)
      message+=default_build_path.string();
    if(has_debug_build) {
      if(has_default_build)
        message+=" and ";
      message+=debug_build_path.string();
    }
    dialog.set_secondary_text(message+"?");
    if(dialog.run()!=Gtk::RESPONSE_YES)
      return;
    try {
      if(has_default_build)
        boost::filesystem::remove_all(default_build_path);
      if(has_debug_build)
        boost::filesystem::remove_all(debug_build_path);
    }
    catch(const std::exception &e) {
      Terminal::get().print(std::string("Error: could not remove build: ")+e.what()+"\n", true);
      return;
    }
  }
  
  build->update_default(true);
  if(has_debug_build)
    build->update_debug(true);
  
  for(size_t c=0;c<Notebook::get().size();c++) {
    auto source_view=Notebook::get().get_view(c);
    if(auto source_clang_view=dynamic_cast<Source::ClangView*>(source_view)) {
      if(filesystem::file_in_path(source_clang_view->file_path, build->project_path))
        source_clang_view->full_reparse_needed=true;
    }
  }
  
  if(auto view=Notebook::get().get_current_view()) {
    if(view->full_reparse_needed)
      view->full_reparse();
  }
}

#ifdef JUCI_ENABLE_DEBUG
std::pair<std::string, std::string> Project::Clang::debug_get_run_arguments() {
  auto debug_build_path=build->get_debug_path();
  auto default_build_path=build->get_default_path();
  if(debug_build_path.empty() || default_build_path.empty())
    return {"", ""};
  
  auto project_path=build->project_path.string();
  auto run_arguments_it=debug_run_arguments.find(project_path);
  std::string arguments;
  if(run_arguments_it!=debug_run_arguments.end())
    arguments=run_arguments_it->second;
  
  if(arguments.empty()) {
    auto view=Notebook::get().get_current_view();
    auto executable=build->get_executable(view?view->file_path:Directories::get().path).string();
    
    if(!executable.empty()) {
      size_t pos=executable.find(default_build_path.string());
      if(pos!=std::string::npos)
        executable.replace(pos, default_build_path.string().size(), debug_build_path.string());
      arguments=filesystem::escape_argument(filesystem::get_short_path(executable).string());
    }
    else
      arguments=filesystem::escape_argument(filesystem::get_short_path(build->get_debug_path()).string());
  }
  
  return {project_path, arguments};
}

Gtk::Popover *Project::Clang::debug_get_options() {
  if(!build->project_path.empty())
    return &debug_options[build->project_path.string()];
  return nullptr;
}

void Project::Clang::debug_start() {
  auto debug_build_path=build->get_debug_path();
  auto default_build_path=build->get_default_path();
  if(debug_build_path.empty() || !build->update_debug() || default_build_path.empty())
    return;
  
  auto project_path=std::make_shared<boost::filesystem::path>(build->project_path);
  
  auto run_arguments_it=debug_run_arguments.find(project_path->string());
  auto run_arguments=std::make_shared<std::string>();
  if(run_arguments_it!=debug_run_arguments.end())
    *run_arguments=run_arguments_it->second;
  
  if(run_arguments->empty()) {
    auto view=Notebook::get().get_current_view();
    *run_arguments=build->get_executable(view?view->file_path:Directories::get().path).string();
    if(run_arguments->empty()) {
      Terminal::get().print("Warning: could not find executable.\n");
      Terminal::get().print("Solution: either use Debug Set Run Arguments, or open a source file within a directory where an executable is defined.\n", true);
      return;
    }
    size_t pos=run_arguments->find(default_build_path.string());
    if(pos!=std::string::npos)
      run_arguments->replace(pos, default_build_path.string().size(), debug_build_path.string());
    *run_arguments=filesystem::escape_argument(filesystem::get_short_path(*run_arguments).string());
  }
  
  if(Config::get().project.clear_terminal_on_compile)
    Terminal::get().clear();
  
  debugging=true;
  Terminal::get().print("Compiling and debugging "+*run_arguments+"\n");
  std::string compile_command;
  if(dynamic_cast<CMakeBuild*>(build.get()))
    compile_command=Config::get().project.cmake.compile_command;
  else if(dynamic_cast<MesonBuild*>(build.get()))
    compile_command=Config::get().project.meson.compile_command;
  Terminal::get().async_process(compile_command, debug_build_path, [this, run_arguments, project_path](int exit_status){
    if(exit_status!=EXIT_SUCCESS)
      debugging=false;
    else {
      dispatcher.post([this, run_arguments, project_path] {
        std::vector<std::pair<boost::filesystem::path, int> > breakpoints;
        for(size_t c=0;c<Notebook::get().size();c++) {
          auto view=Notebook::get().get_view(c);
          if(filesystem::file_in_path(view->file_path, *project_path)) {
            auto iter=view->get_buffer()->begin();
            if(view->get_source_buffer()->get_source_marks_at_iter(iter, "debug_breakpoint").size()>0)
              breakpoints.emplace_back(view->file_path, iter.get_line()+1);
            while(view->get_source_buffer()->forward_iter_to_source_mark(iter, "debug_breakpoint"))
              breakpoints.emplace_back(view->file_path, iter.get_line()+1);
          }
        }
        
        std::string remote_host;
        auto options_it=debug_options.find(project_path->string());
        if(options_it!=debug_options.end() && options_it->second.remote_enabled.get_active())
          remote_host=options_it->second.remote_host.get_text();
        Debug::LLDB::get().start(*run_arguments, *project_path, breakpoints, [this, run_arguments](int exit_status){
          debugging=false;
          Terminal::get().async_print(*run_arguments+" returned: "+std::to_string(exit_status)+'\n');
        }, [this](const std::string &status) {
          dispatcher.post([this, status] {
            debug_update_status(status);
          });
        }, [this](const boost::filesystem::path &file_path, int line_nr, int line_index) {
          dispatcher.post([this, file_path, line_nr, line_index] {
            Project::debug_stop.first=file_path;
            Project::debug_stop.second.first=line_nr-1;
            Project::debug_stop.second.second=line_index-1;
            
            debug_update_stop();
            if(auto view=Notebook::get().get_current_view())
              view->get_buffer()->place_cursor(view->get_buffer()->get_insert()->get_iter());
          });
        }, remote_host);
      });
    }
  });
}

void Project::Clang::debug_continue() {
  Debug::LLDB::get().continue_debug();
}

void Project::Clang::debug_stop() {
  if(debugging)
    Debug::LLDB::get().stop();
}

void Project::Clang::debug_kill() {
  if(debugging)
    Debug::LLDB::get().kill();
}

void Project::Clang::debug_step_over() {
  if(debugging)
    Debug::LLDB::get().step_over();
}

void Project::Clang::debug_step_into() {
  if(debugging)
    Debug::LLDB::get().step_into();
}

void Project::Clang::debug_step_out() {
  if(debugging)
    Debug::LLDB::get().step_out();
}

void Project::Clang::debug_backtrace() {
  if(debugging) {
    auto view=Notebook::get().get_current_view();
    auto backtrace=Debug::LLDB::get().get_backtrace();
    
    if(view) {
      auto iter=view->get_iter_for_dialog();
      SelectionDialog::create(view, view->get_buffer()->create_mark(iter), true, true);
    }
    else
      SelectionDialog::create(true, true);
    auto rows=std::make_shared<std::unordered_map<std::string, Debug::LLDB::Frame> >();
    if(backtrace.size()==0) {
      Info::get().print("No backtrace found");
      return;
    }
    
    bool cursor_set=false;
    for(auto &frame: backtrace) {
      std::string row="<i>"+frame.module_filename+"</i>";
      
      //Shorten frame.function_name if it is too long
      if(frame.function_name.size()>120) {
        frame.function_name=frame.function_name.substr(0, 58)+"...."+frame.function_name.substr(frame.function_name.size()-58);
      }
      if(frame.file_path.empty())
        row+=" - "+Glib::Markup::escape_text(frame.function_name);
      else {
        auto file_path=frame.file_path.filename().string();
        row+=":<b>"+Glib::Markup::escape_text(file_path)+":"+std::to_string(frame.line_nr)+"</b> - "+Glib::Markup::escape_text(frame.function_name);
      }
      (*rows)[row]=frame;
      SelectionDialog::get()->add_row(row);
      if(!cursor_set && view && frame.file_path==view->file_path) {
        SelectionDialog::get()->set_cursor_at_last_row();
        cursor_set=true;
      }
    }
    
    SelectionDialog::get()->on_select=[this, rows](const std::string& selected, bool hide_window) {
      auto frame=rows->at(selected);
      if(!frame.file_path.empty()) {
        Notebook::get().open(frame.file_path);
        if(auto view=Notebook::get().get_current_view()) {
          Debug::LLDB::get().select_frame(frame.index);
          
          view->place_cursor_at_line_index(frame.line_nr-1, frame.line_index-1);
          view->scroll_to_cursor_delayed(view, true, true);
        }
      }
    };
    if(view)
      view->hide_tooltips();
    SelectionDialog::get()->show();
  }
}

void Project::Clang::debug_show_variables() {
  if(debugging) {
    auto view=Notebook::get().get_current_view();
    auto variables=Debug::LLDB::get().get_variables();
    
    Gtk::TextIter iter;
    if(view) {
      iter=view->get_iter_for_dialog();
      SelectionDialog::create(view, view->get_buffer()->create_mark(iter), true, true);
    }
    else
      SelectionDialog::create(true, true);
    auto rows=std::make_shared<std::unordered_map<std::string, Debug::LLDB::Variable> >();
    if(variables.size()==0) {
      Info::get().print("No variables found");
      return;
    }
    
    for(auto &variable: variables) {
      std::string row="#"+std::to_string(variable.thread_index_id)+":#"+std::to_string(variable.frame_index)+":"+variable.file_path.filename().string()+":"+std::to_string(variable.line_nr)+" - <b>"+Glib::Markup::escape_text(variable.name)+"</b>";
      
      (*rows)[row]=variable;
      SelectionDialog::get()->add_row(row);
    }
    
    SelectionDialog::get()->on_select=[this, rows](const std::string& selected, bool hide_window) {
      auto variable=rows->at(selected);
      Debug::LLDB::get().select_frame(variable.frame_index, variable.thread_index_id);
      if(!variable.file_path.empty()) {
        Notebook::get().open(variable.file_path);
        if(auto view=Notebook::get().get_current_view()) {
          view->place_cursor_at_line_index(variable.line_nr-1, variable.line_index-1);
          view->scroll_to_cursor_delayed(view, true, true);
        }
      }
      if(!variable.declaration_found)
        Info::get().print("Debugger did not find declaration for the variable: "+variable.name);
    };
    
    SelectionDialog::get()->on_hide=[this]() {
      debug_variable_tooltips.hide();
      debug_variable_tooltips.clear();
    };
    
    SelectionDialog::get()->on_changed=[this, rows, view, iter](const std::string &selected) {
      if(selected.empty()) {
        debug_variable_tooltips.hide();
        return;
      }
      debug_variable_tooltips.clear();
      auto create_tooltip_buffer=[this, rows, view, selected]() {
        auto variable=rows->at(selected);
        auto tooltip_buffer=view?Gtk::TextBuffer::create(view->get_buffer()->get_tag_table()):Gtk::TextBuffer::create();
        
        Glib::ustring value=variable.value;
        if(!value.empty()) {
          Glib::ustring::iterator iter;
          while(!value.validate(iter)) {
            auto next_char_iter=iter;
            next_char_iter++;
            value.replace(iter, next_char_iter, "?");
          }
          if(view)
            tooltip_buffer->insert_with_tag(tooltip_buffer->get_insert()->get_iter(), value.substr(0, value.size()-1), "def:note");
          else
            tooltip_buffer->insert(tooltip_buffer->get_insert()->get_iter(), value.substr(0, value.size()-1));
        }
        
        return tooltip_buffer;
      };
      
      if(view)
        debug_variable_tooltips.emplace_back(create_tooltip_buffer, view, view->get_buffer()->create_mark(iter), view->get_buffer()->create_mark(iter));
      else
        debug_variable_tooltips.emplace_back(create_tooltip_buffer);
  
      debug_variable_tooltips.show(true);
    };
    
    if(view)
      view->hide_tooltips();
    SelectionDialog::get()->show();
  }
}

void Project::Clang::debug_run_command(const std::string &command) {
  if(debugging) {
    auto command_return=Debug::LLDB::get().run_command(command);
    Terminal::get().async_print(command_return.first);
    Terminal::get().async_print(command_return.second, true);
  }
}

void Project::Clang::debug_add_breakpoint(const boost::filesystem::path &file_path, int line_nr) {
  Debug::LLDB::get().add_breakpoint(file_path, line_nr);
}

void Project::Clang::debug_remove_breakpoint(const boost::filesystem::path &file_path, int line_nr, int line_count) {
  Debug::LLDB::get().remove_breakpoint(file_path, line_nr, line_count);
}

bool Project::Clang::debug_is_running() {
  return Debug::LLDB::get().is_running();
}

void Project::Clang::debug_write(const std::string &buffer) {
  Debug::LLDB::get().write(buffer);
}

void Project::Clang::debug_cancel() {
  Debug::LLDB::get().cancel();
}
#endif

Project::Markdown::~Markdown() {
  if(!last_temp_path.empty()) {
    boost::filesystem::remove(last_temp_path);
    last_temp_path=boost::filesystem::path();
  }
}

void Project::Markdown::compile_and_run() {
  if(!last_temp_path.empty()) {
    boost::filesystem::remove(last_temp_path);
    last_temp_path=boost::filesystem::path();
  }
  
  std::stringstream stdin_stream, stdout_stream;
  auto exit_status=Terminal::get().process(stdin_stream, stdout_stream, "command -v grip");
  if(exit_status==0) {
    auto command="grip -b "+filesystem::escape_argument(filesystem::get_short_path(Notebook::get().get_current_view()->file_path).string());
    Terminal::get().print("Running: "+command+" in a quiet background process\n");
    Terminal::get().async_process(command, "", nullptr, true);
  }
  else
    Terminal::get().print("Warning: install grip to preview Markdown files\n");
}

void Project::Python::compile_and_run() {
  auto command="PYTHONUNBUFFERED=1 python "+filesystem::escape_argument(filesystem::get_short_path(Notebook::get().get_current_view()->file_path).string());
  Terminal::get().print("Running "+command+"\n");
  Terminal::get().async_process(command, Notebook::get().get_current_view()->file_path.parent_path(), [command](int exit_status) {
    Terminal::get().async_print(command+" returned: "+std::to_string(exit_status)+'\n');
  });
}

void Project::JavaScript::compile_and_run() {
  auto command="node --harmony "+filesystem::escape_argument(filesystem::get_short_path(Notebook::get().get_current_view()->file_path).string());
  Terminal::get().print("Running "+command+"\n");
  Terminal::get().async_process(command, Notebook::get().get_current_view()->file_path.parent_path(), [command](int exit_status) {
    Terminal::get().async_print(command+" returned: "+std::to_string(exit_status)+'\n');
  });
}

void Project::HTML::compile_and_run() {
  auto uri=Notebook::get().get_current_view()->file_path.string();
#ifdef __APPLE__
  Terminal::get().process("open "+filesystem::escape_argument(uri));
#else
#ifdef __linux
  uri="file://"+uri;
#endif
  GError* error=nullptr;
#if GTK_VERSION_GE(3, 22)
  gtk_show_uri_on_window(nullptr, uri.c_str(), GDK_CURRENT_TIME, &error);
#else
  gtk_show_uri(nullptr, uri.c_str(), GDK_CURRENT_TIME, &error);
#endif
  g_clear_error(&error);
#endif
}
